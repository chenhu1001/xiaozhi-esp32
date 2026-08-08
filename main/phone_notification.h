#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

enum class PhoneNotificationEvent {
    Added,
    Modified,
    Removed,
};

struct PhoneNotification {
    uint32_t uid = 0;
    PhoneNotificationEvent event = PhoneNotificationEvent::Added;
    std::string app_identifier;
    std::string title;
    std::string subtitle;
    std::string message;
    bool truncated = false;

    std::string ToDisplayText() const;
    std::string ToSpeechText() const;
};

// Owns only the current boot's notification history. Callers invoke Publish on
// the application task so UI and speech callbacks never run on NimBLE's task.
class NotificationController {
public:
    using NotificationCallback = std::function<void(const PhoneNotification&)>;

    NotificationController(NotificationCallback on_present, NotificationCallback on_speak);

    void Publish(PhoneNotification notification);
    void Remove(uint32_t uid);

private:
    static constexpr size_t kMaxHistory = 10;

    NotificationCallback on_present_;
    NotificationCallback on_speak_;
    std::deque<PhoneNotification> history_;
    std::mutex mutex_;
};
