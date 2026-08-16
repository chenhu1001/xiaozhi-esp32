#pragma once

#include "ancs_notification.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_set>

class AncsPreExistingFilter {
public:
    explicit AncsPreExistingFilter(size_t capacity) : capacity_(capacity) {}

    bool ShouldIgnore(const AncsNotification& notification) {
        if (IsAncsPreExisting(notification.event_flags)) {
            Remember(notification.uid);
            return true;
        }
        if (uids_.find(notification.uid) == uids_.end()) {
            return false;
        }
        if (notification.event == AncsEventType::kRemoved) {
            Forget(notification.uid);
        }
        return true;
    }

    void Clear() {
        order_.clear();
        uids_.clear();
    }

private:
    void Remember(uint32_t uid) {
        if (capacity_ == 0 || uids_.find(uid) != uids_.end()) {
            return;
        }
        while (order_.size() >= capacity_) {
            uids_.erase(order_.front());
            order_.pop_front();
        }
        uids_.insert(uid);
        order_.push_back(uid);
    }

    void Forget(uint32_t uid) {
        uids_.erase(uid);
        for (auto iterator = order_.begin(); iterator != order_.end(); ++iterator) {
            if (*iterator == uid) {
                order_.erase(iterator);
                return;
            }
        }
    }

    size_t capacity_;
    std::deque<uint32_t> order_;
    std::unordered_set<uint32_t> uids_;
};
