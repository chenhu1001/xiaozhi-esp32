#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum class AncsEventType {
    kAdded,
    kModified,
    kRemoved,
};

struct AncsNotification {
    AncsEventType event = AncsEventType::kAdded;
    std::string session_id;
    uint64_t sequence = 0;
    uint32_t uid = 0;
    uint8_t category_id = 0;
    uint8_t event_flags = 0;
    std::string app_identifier;
    std::string title;
    std::string subtitle;
    std::string message;
    bool truncated = false;
};

inline bool ParseAncsNotificationRecord(const uint8_t* record, size_t size,
                                        AncsNotification& notification) {
    if (record == nullptr || size != 8) {
        return false;
    }
    switch (record[0]) {
        case 0:
            notification.event = AncsEventType::kAdded;
            break;
        case 1:
            notification.event = AncsEventType::kModified;
            break;
        case 2:
            notification.event = AncsEventType::kRemoved;
            break;
        default:
            return false;
    }
    notification.event_flags = record[1];
    notification.category_id = record[2];
    notification.uid = static_cast<uint32_t>(record[4]) | (static_cast<uint32_t>(record[5]) << 8) |
                       (static_cast<uint32_t>(record[6]) << 16) |
                       (static_cast<uint32_t>(record[7]) << 24);
    return true;
}

inline bool IsAncsPreExisting(uint8_t event_flags) { return (event_flags & (1 << 2)) != 0; }
