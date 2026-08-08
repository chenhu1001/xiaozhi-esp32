#include "ancs_client.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

namespace {

constexpr char kTag[] = "AncsClient";
constexpr uint8_t kEventAdded = 0;
constexpr uint8_t kEventModified = 1;
constexpr uint8_t kEventRemoved = 2;
constexpr uint8_t kCommandGetNotificationAttributes = 0;
constexpr uint8_t kAttributeAppIdentifier = 0;
constexpr uint8_t kAttributeTitle = 1;
constexpr uint8_t kAttributeSubtitle = 2;
constexpr uint8_t kAttributeMessage = 3;
constexpr uint16_t kTitleMaxLength = 256;
constexpr uint16_t kSubtitleMaxLength = 256;
constexpr uint16_t kMessageMaxLength = 1024;
constexpr int64_t kDataSourceTimeoutUs = 150000;
constexpr char kDeviceName[] = "Xiaozhi ANCS";

// The iOS companion app scans this service before connecting with ANCS authorization.
const ble_uuid128_t kCompanionServiceUuid = BLE_UUID128_INIT(
    0xE5, 0xD4, 0xC3, 0xC2, 0x7D, 0x03, 0xA6, 0x86, 0x75, 0x4D, 0x0C, 0x4E, 0xFE, 0x45, 0xC8, 0xB3);
const ble_uuid128_t kCompanionStatusUuid = BLE_UUID128_INIT(
    0xE6, 0xD4, 0xC3, 0xC2, 0x7D, 0x03, 0xA6, 0x86, 0x75, 0x4D, 0x0C, 0x4E, 0xFE, 0x45, 0xC8, 0xB3);

const ble_uuid128_t kAncsServiceUuid = BLE_UUID128_INIT(
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, 0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79);
const ble_uuid128_t kNotificationSourceUuid = BLE_UUID128_INIT(
    0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C, 0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
const ble_uuid128_t kControlPointUuid = BLE_UUID128_INIT(
    0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98, 0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69);
const ble_uuid128_t kDataSourceUuid = BLE_UUID128_INIT(
    0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE, 0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22);
const ble_uuid16_t kClientConfigurationUuid = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);

uint32_t ReadLittleEndian32(const uint8_t* value) {
    return static_cast<uint32_t>(value[0]) | (static_cast<uint32_t>(value[1]) << 8) |
           (static_cast<uint32_t>(value[2]) << 16) | (static_cast<uint32_t>(value[3]) << 24);
}

uint16_t ReadLittleEndian16(const uint8_t* value) {
    return static_cast<uint16_t>(value[0]) | (static_cast<uint16_t>(value[1]) << 8);
}

int CompanionStatusAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* context, void*) {
    if (context->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(context->om, "ANCS", 4) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

const ble_gatt_chr_def kCompanionCharacteristics[] = {
    {
        .uuid = &kCompanionStatusUuid.u,
        .access_cb = CompanionStatusAccess,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {},
};

const ble_gatt_svc_def kCompanionServices[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kCompanionServiceUuid.u,
        .characteristics = kCompanionCharacteristics,
    },
    {},
};

}  // namespace

AncsClient* AncsClient::instance_ = nullptr;

AncsClient::AncsClient(NotificationCallback callback) : on_notification_(std::move(callback)) {}

AncsClient::~AncsClient() { Stop(); }

bool AncsClient::Start() {
    if (started_) {
        return true;
    }
    if (instance_ != nullptr) {
        ESP_LOGE(kTag, "Only one ANCS client can run");
        return false;
    }

    const esp_err_t result = nimble_port_init();
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "nimble_port_init failed: %s", esp_err_to_name(result));
        return false;
    }

    instance_ = this;
    ble_hs_cfg.reset_cb = OnReset;
    ble_hs_cfg.sync_cb = OnSync;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(kDeviceName);
    ESP_ERROR_CHECK(ble_gatts_count_cfg(kCompanionServices));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(kCompanionServices));
    esp_timer_create_args_t timer_args = {
        .callback = OnDataSourceTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ancs_data",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    data_timer_ = timer;
    data_timer_created_ = true;

    started_ = true;
    nimble_port_freertos_init(HostTask);
    return true;
}

void AncsClient::Stop() {
    if (!started_) {
        return;
    }
    if (connected_) {
        ble_gap_terminate(connection_handle_, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_adv_stop();
    nimble_port_stop();
    esp_nimble_deinit();
    if (data_timer_created_) {
        auto timer = static_cast<esp_timer_handle_t>(data_timer_);
        esp_timer_stop(timer);
        esp_timer_delete(timer);
    }
    data_timer_ = nullptr;
    data_timer_created_ = false;
    started_ = false;
    instance_ = nullptr;
}

void AncsClient::StartAdvertising() {
    if (!started_ || connected_) {
        return;
    }

    ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &kCompanionServiceUuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        ESP_LOGW(kTag, "Unable to set ANCS advertising fields: %d", result);
        return;
    }

    // A complete 128-bit UUID and the device name exceed the 31-byte advertising limit.
    ble_hs_adv_fields scan_response = {};
    scan_response.name = reinterpret_cast<const uint8_t*>(kDeviceName);
    scan_response.name_len = sizeof(kDeviceName) - 1;
    scan_response.name_is_complete = 1;
    result = ble_gap_adv_rsp_set_fields(&scan_response);
    if (result != 0) {
        ESP_LOGW(kTag, "Unable to set ANCS scan response: %d", result);
        return;
    }

    ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    result = ble_gap_adv_start(own_addr_type_, nullptr, BLE_HS_FOREVER, &params, GapEvent, this);
    if (result != 0) {
        ESP_LOGW(kTag, "Unable to start ANCS advertising: %d", result);
    } else {
        ESP_LOGI(kTag, "Advertising for the iOS ANCS companion app");
    }
}

void AncsClient::DiscoverAncs(uint16_t conn_handle) {
    const int result =
        ble_gattc_disc_svc_by_uuid(conn_handle, &kAncsServiceUuid.u, ServiceDiscovered, this);
    if (result != 0) {
        ESP_LOGW(kTag, "ANCS service discovery failed: %d", result);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void AncsClient::RequestNotificationAttributes(uint32_t uid) {
    if (!ancs_ready_ || control_point_handle_ == 0) {
        return;
    }
    std::array<uint8_t, 16> command = {
        kCommandGetNotificationAttributes,
        static_cast<uint8_t>(uid),
        static_cast<uint8_t>(uid >> 8),
        static_cast<uint8_t>(uid >> 16),
        static_cast<uint8_t>(uid >> 24),
        kAttributeAppIdentifier,
        kAttributeTitle,
        static_cast<uint8_t>(kTitleMaxLength),
        static_cast<uint8_t>(kTitleMaxLength >> 8),
        kAttributeSubtitle,
        static_cast<uint8_t>(kSubtitleMaxLength),
        static_cast<uint8_t>(kSubtitleMaxLength >> 8),
        kAttributeMessage,
        static_cast<uint8_t>(kMessageMaxLength),
        static_cast<uint8_t>(kMessageMaxLength >> 8),
    };
    pending_uid_ = uid;
    data_source_buffer_.clear();
    const int result = ble_gattc_write_flat(connection_handle_, control_point_handle_,
                                            command.data(), command.size(), nullptr, nullptr);
    if (result != 0) {
        ESP_LOGW(kTag, "Failed to request ANCS attributes: %d", result);
    }
}

void AncsClient::QueueNotificationAttributes(uint32_t uid) {
    if (pending_uid_ == 0) {
        RequestNotificationAttributes(uid);
        return;
    }
    if (std::find(pending_requests_.begin(), pending_requests_.end(), uid) !=
        pending_requests_.end()) {
        return;
    }
    if (pending_requests_.size() == 10) {
        pending_requests_.pop_front();
    }
    pending_requests_.push_back(uid);
}

void AncsClient::ProcessDataSource() {
    if (data_source_buffer_.size() < 5 ||
        data_source_buffer_[0] != kCommandGetNotificationAttributes) {
        return;
    }

    const uint32_t uid = ReadLittleEndian32(data_source_buffer_.data() + 1);
    if (uid != pending_uid_) {
        ESP_LOGW(kTag, "Discarding ANCS response for unexpected UID");
        data_source_buffer_.clear();
        return;
    }

    PhoneNotification notification;
    notification.uid = uid;
    notification.event = PhoneNotificationEvent::Added;
    size_t offset = 5;
    while (offset + 3 <= data_source_buffer_.size()) {
        const uint8_t attribute = data_source_buffer_[offset];
        const uint16_t length = ReadLittleEndian16(data_source_buffer_.data() + offset + 1);
        offset += 3;
        if (offset + length > data_source_buffer_.size()) {
            return;
        }
        const std::string value(reinterpret_cast<const char*>(data_source_buffer_.data() + offset),
                                length);
        switch (attribute) {
            case kAttributeAppIdentifier:
                notification.app_identifier = value;
                break;
            case kAttributeTitle:
                notification.title = value;
                notification.truncated = length >= kTitleMaxLength;
                break;
            case kAttributeSubtitle:
                notification.subtitle = value;
                notification.truncated = notification.truncated || length >= kSubtitleMaxLength;
                break;
            case kAttributeMessage:
                notification.message = value;
                notification.truncated = notification.truncated || length >= kMessageMaxLength;
                break;
            default:
                break;
        }
        offset += length;
    }

    if (offset == data_source_buffer_.size()) {
        pending_uid_ = 0;
        data_source_buffer_.clear();
        if (on_notification_) {
            on_notification_(std::move(notification));
        }
        if (!pending_requests_.empty()) {
            const uint32_t next_uid = pending_requests_.front();
            pending_requests_.pop_front();
            RequestNotificationAttributes(next_uid);
        }
    }
}

void AncsClient::PublishPendingNotification() {
    ProcessDataSource();
    if (!data_source_buffer_.empty()) {
        ESP_LOGW(kTag, "Timed out while collecting ANCS notification attributes");
        data_source_buffer_.clear();
        pending_uid_ = 0;
        if (!pending_requests_.empty()) {
            const uint32_t next_uid = pending_requests_.front();
            pending_requests_.pop_front();
            RequestNotificationAttributes(next_uid);
        }
    }
}

void AncsClient::ResetConnection() {
    connected_ = false;
    ancs_ready_ = false;
    connection_handle_ = 0xffff;
    service_end_handle_ = 0;
    notification_source_handle_ = 0;
    data_source_handle_ = 0;
    control_point_handle_ = 0;
    pending_uid_ = 0;
    pending_requests_.clear();
    data_source_buffer_.clear();
}

int AncsClient::GapEvent(struct ble_gap_event* event, void* arg) {
    auto* client = static_cast<AncsClient*>(arg);
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ESP_LOGW(kTag, "iOS ANCS app connection failed: %d", event->connect.status);
                client->StartAdvertising();
                return 0;
            }
            client->connected_ = true;
            client->connection_handle_ = event->connect.conn_handle;
            ESP_LOGI(kTag, "iOS app connected, requesting ANCS pairing and encryption");
            if (const int rc = ble_gap_security_initiate(event->connect.conn_handle);
                rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(kTag, "Failed to initiate ANCS security: %d", rc);
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0) {
                ESP_LOGI(kTag, "ANCS link encrypted, discovering iPhone service");
                client->DiscoverAncs(event->enc_change.conn_handle);
            } else {
                ESP_LOGW(kTag, "ANCS encryption failed: %d", event->enc_change.status);
                ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(kTag, "ANCS disconnected: %d", event->disconnect.reason);
            client->ResetConnection();
            client->StartAdvertising();
            return 0;
        case BLE_GAP_EVENT_NOTIFY_RX:
            if (event->notify_rx.attr_handle == client->notification_source_handle_) {
                const auto* data = event->notify_rx.om->om_data;
                const size_t length = event->notify_rx.om->om_len;
                if (length < 8) {
                    return 0;
                }
                const uint32_t uid = ReadLittleEndian32(data + 4);
                if (data[0] == kEventRemoved) {
                    if (client->on_notification_) {
                        PhoneNotification removed;
                        removed.uid = uid;
                        removed.event = PhoneNotificationEvent::Removed;
                        client->on_notification_(std::move(removed));
                    }
                } else if (data[0] == kEventAdded || data[0] == kEventModified) {
                    client->QueueNotificationAttributes(uid);
                }
            } else if (event->notify_rx.attr_handle == client->data_source_handle_) {
                const size_t length = event->notify_rx.om->om_len;
                if (client->data_source_buffer_.size() + length > kMaxDataSourceBytes) {
                    ESP_LOGW(kTag, "ANCS notification data exceeds local limit");
                    client->data_source_buffer_.clear();
                    client->pending_uid_ = 0;
                    return 0;
                }
                const auto* data = event->notify_rx.om->om_data;
                client->data_source_buffer_.insert(client->data_source_buffer_.end(), data,
                                                   data + length);
                client->ProcessDataSource();
                if (!client->data_source_buffer_.empty()) {
                    auto timer = static_cast<esp_timer_handle_t>(client->data_timer_);
                    esp_timer_stop(timer);
                    esp_timer_start_once(timer, kDataSourceTimeoutUs);
                }
            }
            return 0;
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(kTag, "ANCS MTU updated to %d", event->mtu.value);
            return 0;
        default:
            return 0;
    }
}

int AncsClient::ServiceDiscovered(uint16_t conn_handle, const ble_gatt_error* error,
                                  const ble_gatt_svc* service, void* arg) {
    auto* client = static_cast<AncsClient*>(arg);
    if (error->status != 0) {
        ESP_LOGW(kTag, "ANCS service discovery finished: %d", error->status);
        return 0;
    }
    client->service_end_handle_ = service->end_handle;
    return ble_gattc_disc_all_chrs(conn_handle, service->start_handle, service->end_handle,
                                   CharacteristicDiscovered, client);
}

int AncsClient::CharacteristicDiscovered(uint16_t conn_handle, const ble_gatt_error* error,
                                         const ble_gatt_chr* characteristic, void* arg) {
    auto* client = static_cast<AncsClient*>(arg);
    if (error->status != 0) {
        client->ancs_ready_ = client->notification_source_handle_ != 0 &&
                              client->data_source_handle_ != 0 &&
                              client->control_point_handle_ != 0;
        ESP_LOGI(kTag, "ANCS characteristics %s", client->ancs_ready_ ? "ready" : "incomplete");
        if (client->ancs_ready_) {
            return ble_gattc_disc_all_dscs(conn_handle, client->notification_source_handle_,
                                           client->service_end_handle_, DescriptorDiscovered,
                                           client);
        }
        return 0;
    }

    if (ble_uuid_cmp(&characteristic->uuid.u, &kNotificationSourceUuid.u) == 0) {
        client->notification_source_handle_ = characteristic->val_handle;
    } else if (ble_uuid_cmp(&characteristic->uuid.u, &kDataSourceUuid.u) == 0) {
        client->data_source_handle_ = characteristic->val_handle;
    } else if (ble_uuid_cmp(&characteristic->uuid.u, &kControlPointUuid.u) == 0) {
        client->control_point_handle_ = characteristic->val_handle;
    }
    return 0;
}

int AncsClient::DescriptorDiscovered(uint16_t conn_handle, const ble_gatt_error* error,
                                     uint16_t characteristic_handle, const ble_gatt_dsc* descriptor,
                                     void* arg) {
    auto* client = static_cast<AncsClient*>(arg);
    if (error->status != 0 || ble_uuid_cmp(&descriptor->uuid.u, &kClientConfigurationUuid.u) != 0) {
        return 0;
    }
    const uint8_t enabled[] = {0x01, 0x00};
    return ble_gattc_write_flat(conn_handle, descriptor->handle, enabled, sizeof(enabled),
                                SubscriptionComplete, client);
}

int AncsClient::SubscriptionComplete(uint16_t, const ble_gatt_error* error, ble_gatt_attr*, void*) {
    if (error->status != 0) {
        ESP_LOGW(kTag, "ANCS subscription failed: %d", error->status);
    }
    return 0;
}

void AncsClient::OnDataSourceTimeout(void* arg) {
    static_cast<AncsClient*>(arg)->PublishPendingNotification();
}

void AncsClient::OnReset(int reason) { ESP_LOGW(kTag, "NimBLE reset: %d", reason); }

void AncsClient::OnSync() {
    if (instance_ == nullptr) {
        return;
    }
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &instance_->own_addr_type_) != 0) {
        ESP_LOGE(kTag, "Unable to determine BLE address type");
        return;
    }
    instance_->StartAdvertising();
}

void AncsClient::HostTask(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}
