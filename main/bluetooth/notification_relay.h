#pragma once

#include "ancs_notification.h"
#include "ancs_relay_policy.h"
#include "ancs_webhook.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

class NotificationRelay {
public:
    using DeliveryDecision = AncsDeliveryDecision;

    NotificationRelay() = default;
    ~NotificationRelay();

    NotificationRelay(const NotificationRelay&) = delete;
    NotificationRelay& operator=(const NotificationRelay&) = delete;

    bool Start();
    void Stop();
    void SetNetworkConnected(bool connected);
    void Enqueue(AncsNotification notification);

    static DeliveryDecision ClassifyHttpStatus(int status_code);

private:
    static constexpr size_t kMaxQueueSize = 20;

    struct QueueItem {
        AncsNotification notification;
        unsigned retry_count = 0;
        bool logged = false;
    };

    static void TaskEntry(void* arg);
    void Run();
    DeliveryDecision Deliver(const AncsWebhookPayload& item);
    AncsWebhookPayload Serialize(const AncsNotification& notification) const;
    static bool IsSameEvent(const QueueItem& item, const AncsNotification& notification);
    bool RemoveIfPresent(const AncsNotification& notification);
    bool WaitForRetry(const AncsNotification& notification, TickType_t delay);
    void LogPendingEvents();

    std::mutex mutex_;
    std::deque<QueueItem> queue_;
    std::atomic<TaskHandle_t> task_handle_{nullptr};
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> network_connected_{false};
    std::atomic<unsigned> dropped_oldest_{0};
};
