#include "ancs_webhook.h"

#include <cJSON.h>

#include <array>
#include <utility>

namespace {

bool AddString(cJSON* object, const char* name, const std::string& value) {
    return cJSON_AddStringToObject(object, name, value.c_str()) != nullptr;
}

}  // namespace

const char* AncsEventName(AncsEventType event) {
    switch (event) {
        case AncsEventType::kAdded:
            return "added";
        case AncsEventType::kModified:
            return "modified";
        case AncsEventType::kRemoved:
            return "removed";
    }
    return "added";
}

const char* AncsCategoryName(uint8_t category_id) {
    static constexpr std::array<const char*, 12> kCategories = {
        "other",    "incoming_call", "missed_call", "voicemail",          "social",
        "schedule", "email",         "news",        "health_and_fitness", "business_and_finance",
        "location", "entertainment",
    };
    return category_id < kCategories.size() ? kCategories[category_id] : "unknown";
}

AncsWebhookPayload SerializeAncsWebhook(const AncsNotification& notification,
                                        const AncsDeviceIdentity& device_identity) {
    const std::string event_id = device_identity.id + ":" + notification.session_id + ":" +
                                 std::to_string(notification.sequence);
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return {.event_id = "", .json = ""};
    }
    if (cJSON_AddNumberToObject(root, "schema_version", 1) == nullptr ||
        cJSON_AddStringToObject(root, "type", "ios_ancs_notification") == nullptr ||
        !AddString(root, "event_id", event_id)) {
        cJSON_Delete(root);
        return {.event_id = "", .json = ""};
    }

    cJSON* device = cJSON_AddObjectToObject(root, "device");
    cJSON* ancs = cJSON_AddObjectToObject(root, "ancs");
    if (device == nullptr || ancs == nullptr) {
        cJSON_Delete(root);
        return {.event_id = "", .json = ""};
    }
    if (!AddString(device, "id", device_identity.id) ||
        !AddString(device, "mac", device_identity.mac) ||
        !AddString(device, "board", device_identity.board) ||
        !AddString(ancs, "session_id", notification.session_id) ||
        cJSON_AddNumberToObject(ancs, "sequence", static_cast<double>(notification.sequence)) ==
            nullptr ||
        cJSON_AddStringToObject(ancs, "event", AncsEventName(notification.event)) == nullptr ||
        cJSON_AddNumberToObject(ancs, "uid", notification.uid) == nullptr ||
        cJSON_AddNumberToObject(ancs, "category_id", notification.category_id) == nullptr ||
        cJSON_AddStringToObject(ancs, "category", AncsCategoryName(notification.category_id)) ==
            nullptr ||
        cJSON_AddNumberToObject(ancs, "event_flags", notification.event_flags) == nullptr) {
        cJSON_Delete(root);
        return {.event_id = "", .json = ""};
    }

    cJSON* flags = cJSON_AddObjectToObject(ancs, "flags");
    if (flags == nullptr) {
        cJSON_Delete(root);
        return {.event_id = "", .json = ""};
    }
    if (cJSON_AddBoolToObject(flags, "silent", notification.event_flags & (1 << 0)) == nullptr ||
        cJSON_AddBoolToObject(flags, "important", notification.event_flags & (1 << 1)) == nullptr ||
        cJSON_AddBoolToObject(flags, "pre_existing", notification.event_flags & (1 << 2)) ==
            nullptr ||
        cJSON_AddBoolToObject(flags, "positive_action", notification.event_flags & (1 << 3)) ==
            nullptr ||
        cJSON_AddBoolToObject(flags, "negative_action", notification.event_flags & (1 << 4)) ==
            nullptr ||
        !AddString(ancs, "app_identifier", notification.app_identifier) ||
        !AddString(ancs, "title", notification.title) ||
        !AddString(ancs, "subtitle", notification.subtitle) ||
        !AddString(ancs, "message", notification.message) ||
        cJSON_AddBoolToObject(ancs, "truncated", notification.truncated) == nullptr) {
        cJSON_Delete(root);
        return {.event_id = "", .json = ""};
    }

    char* printed = cJSON_PrintUnformatted(root);
    std::string json = printed != nullptr ? printed : "";
    cJSON_free(printed);
    cJSON_Delete(root);
    return {.event_id = event_id, .json = std::move(json)};
}
