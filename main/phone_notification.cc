#include "phone_notification.h"

#include <algorithm>

namespace {

void AppendField(std::string& output, const std::string& value) {
    if (value.empty()) {
        return;
    }
    if (!output.empty()) {
        output += "\n";
    }
    output += value;
}

}  // namespace

std::string PhoneNotification::ToDisplayText() const {
    std::string text;
    AppendField(text, title);
    AppendField(text, subtitle);
    AppendField(text, message);
    return text.empty() ? app_identifier : text;
}

std::string PhoneNotification::ToSpeechText() const {
    std::string text;
    AppendField(text, app_identifier);
    AppendField(text, title);
    AppendField(text, subtitle);
    AppendField(text, message);
    return text;
}

NotificationController::NotificationController(NotificationCallback on_present,
                                               NotificationCallback on_speak)
    : on_present_(std::move(on_present)), on_speak_(std::move(on_speak)) {}

void NotificationController::Publish(PhoneNotification notification) {
    if (notification.event == PhoneNotificationEvent::Removed) {
        Remove(notification.uid);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = std::find_if(
            history_.begin(), history_.end(),
            [&notification](const auto& item) { return item.uid == notification.uid; });
        if (existing != history_.end()) {
            *existing = notification;
        } else {
            if (history_.size() == kMaxHistory) {
                history_.pop_front();
            }
            history_.push_back(notification);
        }
    }

    if (on_present_) {
        on_present_(notification);
    }
    if (on_speak_) {
        on_speak_(notification);
    }
}

void NotificationController::Remove(uint32_t uid) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(history_, [uid](const auto& item) { return item.uid == uid; });
}
