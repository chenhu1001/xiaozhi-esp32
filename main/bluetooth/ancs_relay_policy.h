#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

enum class AncsDeliveryDecision {
    kSuccess,
    kRetry,
    kDrop,
};

inline AncsDeliveryDecision ClassifyAncsHttpStatus(int status_code) {
    if (status_code >= 200 && status_code < 300) {
        return AncsDeliveryDecision::kSuccess;
    }
    if (status_code <= 0 || status_code == 408 || status_code == 429 ||
        (status_code >= 500 && status_code < 600)) {
        return AncsDeliveryDecision::kRetry;
    }
    return AncsDeliveryDecision::kDrop;
}

inline uint32_t AncsRetryDelaySeconds(size_t retry_count) {
    constexpr std::array<uint32_t, 6> kDelays = {1, 2, 4, 8, 16, 30};
    return kDelays[retry_count < kDelays.size() ? retry_count : kDelays.size() - 1];
}

template <typename T>
bool PushAncsBoundedFifo(std::deque<T>& queue, size_t capacity, T item) {
    if (capacity == 0) {
        return true;
    }
    const bool dropped_oldest = queue.size() >= capacity;
    if (dropped_oldest) {
        queue.pop_front();
    }
    queue.push_back(std::move(item));
    return dropped_oldest;
}
