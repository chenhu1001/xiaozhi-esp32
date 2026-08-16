#pragma once

#include "ancs_notification.h"

#include <cstdint>
#include <string>

struct AncsDeviceIdentity {
    std::string id;
    std::string mac;
    std::string board;
};

struct AncsWebhookPayload {
    std::string event_id;
    std::string json;
};

const char* AncsEventName(AncsEventType event);
const char* AncsCategoryName(uint8_t category_id);
AncsWebhookPayload SerializeAncsWebhook(const AncsNotification& notification,
                                        const AncsDeviceIdentity& device);
