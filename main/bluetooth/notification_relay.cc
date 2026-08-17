#include "notification_relay.h"

#include "ancs_relay_config.h"
#include "ancs_webhook.h"
#include "board.h"
#include "system_info.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/idf_additions.h>

#include <algorithm>
#include <cinttypes>
#include <utility>
#include <vector>

namespace {

constexpr char kTag[] = "AncsRelay";
constexpr char kUserAgent[] = "xiaozhi-esp32-ancs-relay/1";

}  // namespace

NotificationRelay::~NotificationRelay() { Stop(); }

bool NotificationRelay::Start() {
    if (started_.exchange(true)) {
        return true;
    }
    stopping_ = false;
    TaskHandle_t task_handle = nullptr;
    if (xTaskCreateWithCaps(TaskEntry, "ancs_http", 6144, this, 1, &task_handle,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(kTag, "Unable to create HTTP relay task in PSRAM");
        task_handle_ = nullptr;
        started_ = false;
        return false;
    }
    task_handle_ = task_handle;
    ESP_LOGI(kTag, "HTTP relay task stack allocated in PSRAM");
    return true;
}

void NotificationRelay::Stop() {
    if (!started_.exchange(false)) {
        return;
    }
    stopping_ = true;
    if (TaskHandle_t task_handle = task_handle_.load(); task_handle != nullptr) {
        xTaskNotifyGive(task_handle);
        while (task_handle_.load() != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

void NotificationRelay::SetNetworkConnected(bool connected) {
    network_connected_ = connected;
    if (TaskHandle_t task_handle = task_handle_.load(); task_handle != nullptr) {
        xTaskNotifyGive(task_handle);
    }
}

void NotificationRelay::Enqueue(AncsNotification notification) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (PushAncsBoundedFifo(queue_, kMaxQueueSize,
                                QueueItem{.notification = std::move(notification)})) {
            ++dropped_oldest_;
        }
    }
    if (TaskHandle_t task_handle = task_handle_.load(); task_handle != nullptr) {
        xTaskNotifyGive(task_handle);
    }
}

NotificationRelay::DeliveryDecision NotificationRelay::ClassifyHttpStatus(int status_code) {
    return ClassifyAncsHttpStatus(status_code);
}

void NotificationRelay::TaskEntry(void* arg) {
    auto* relay = static_cast<NotificationRelay*>(arg);
    relay->Run();
    relay->task_handle_ = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

void NotificationRelay::Run() {
    while (!stopping_) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        LogPendingEvents();
        while (!stopping_ && network_connected_) {
            QueueItem item;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty()) {
                    break;
                }
                item = queue_.front();
            }

            const AncsWebhookPayload wire_item = Serialize(item.notification);
            if (wire_item.json.empty()) {
                ESP_LOGE(kTag, "Unable to serialize ANCS event; dropping it");
                RemoveIfPresent(item.notification);
                continue;
            }
            const DeliveryDecision decision = Deliver(wire_item);
            if (decision == DeliveryDecision::kSuccess) {
                ESP_LOGI(kTag, "Forwarded event %s", wire_item.event_id.c_str());
                RemoveIfPresent(item.notification);
                continue;
            }
            if (decision == DeliveryDecision::kDrop) {
                ESP_LOGW(kTag, "Permanent HTTP failure; dropping event %s",
                         wire_item.event_id.c_str());
                RemoveIfPresent(item.notification);
                continue;
            }

            unsigned retry_count = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto found =
                    std::find_if(queue_.begin(), queue_.end(), [&](const QueueItem& queued) {
                        return IsSameEvent(queued, item.notification);
                    });
                if (found == queue_.end()) {
                    continue;
                }
                retry_count = found->retry_count++;
            }
            const uint32_t seconds = AncsRetryDelaySeconds(retry_count);
            ESP_LOGW(kTag, "Retry event %s in %" PRIu32 " seconds", wire_item.event_id.c_str(),
                     seconds);
            if (!WaitForRetry(item.notification, pdMS_TO_TICKS(seconds * 1000))) {
                if (stopping_ || !network_connected_) {
                    break;
                }
                continue;
            }
            LogPendingEvents();
        }
    }
}

NotificationRelay::DeliveryDecision NotificationRelay::Deliver(const AncsWebhookPayload& item) {
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGW(kTag, "Network interface unavailable for event %s", item.event_id.c_str());
        return DeliveryDecision::kRetry;
    }

    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGW(kTag, "Unable to create HTTP client for event %s", item.event_id.c_str());
        return DeliveryDecision::kRetry;
    }
    const std::string mac = SystemInfo::GetMacAddress();
    const std::string client_id = Board::GetInstance().GetUuid();
    http->SetTimeout(15000);
    http->SetKeepAlive(false);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-Id", mac);
    http->SetHeader("Client-Id", client_id);
    http->SetHeader("User-Agent", kUserAgent);
    http->SetContent(std::string(item.json));
    if (!http->Open("POST", kAncsRelayEndpoint)) {
        ESP_LOGW(kTag, "POST failed for event %s: error=%d", item.event_id.c_str(),
                 http->GetLastError());
        return DeliveryDecision::kRetry;
    }
    const int status_code = http->GetStatusCode();
    ESP_LOGI(kTag, "POST event %s returned HTTP %d", item.event_id.c_str(), status_code);
    http->Close();
    return ClassifyHttpStatus(status_code);
}

AncsWebhookPayload NotificationRelay::Serialize(const AncsNotification& notification) const {
    return SerializeAncsWebhook(notification, {.id = Board::GetInstance().GetUuid(),
                                               .mac = SystemInfo::GetMacAddress(),
                                               .board = BOARD_NAME});
}

bool NotificationRelay::IsSameEvent(const QueueItem& item, const AncsNotification& notification) {
    return item.notification.session_id == notification.session_id &&
           item.notification.sequence == notification.sequence;
}

bool NotificationRelay::RemoveIfPresent(const AncsNotification& notification) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = std::find_if(queue_.begin(), queue_.end(), [&](const QueueItem& item) {
        return IsSameEvent(item, notification);
    });
    if (found == queue_.end()) {
        return false;
    }
    queue_.erase(found);
    return true;
}

bool NotificationRelay::WaitForRetry(const AncsNotification& notification, TickType_t delay) {
    const TickType_t started_at = xTaskGetTickCount();
    TickType_t remaining = delay;
    while (!stopping_ && network_connected_ && remaining > 0) {
        ulTaskNotifyTake(pdTRUE, remaining);
        LogPendingEvents();
        const TickType_t elapsed = xTaskGetTickCount() - started_at;
        remaining = elapsed >= delay ? 0 : delay - elapsed;
    }
    if (stopping_ || !network_connected_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(queue_.begin(), queue_.end(),
                       [&](const QueueItem& item) { return IsSameEvent(item, notification); });
}

void NotificationRelay::LogPendingEvents() {
    const unsigned dropped = dropped_oldest_.exchange(0);
    if (dropped != 0) {
        ESP_LOGW(kTag, "Relay queue full; dropped %u oldest ANCS event(s)", dropped);
    }
    std::vector<AncsNotification> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : queue_) {
            if (!item.logged) {
                item.logged = true;
                events.push_back(item.notification);
            }
        }
    }
    for (const auto& notification : events) {
        ESP_LOGI(kTag, "ANCS %s uid=%" PRIu32 " app=%s title=%s message=%s",
                 AncsEventName(notification.event), notification.uid,
                 notification.app_identifier.c_str(), notification.title.c_str(),
                 notification.message.c_str());
    }
}
