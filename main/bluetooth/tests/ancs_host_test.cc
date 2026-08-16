#include "ancs_attribute_parser.h"
#include "ancs_preexisting_filter.h"
#include "ancs_relay_policy.h"
#include "ancs_webhook.h"

#include <cJSON.h>

#include <cassert>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace {

void Append16(std::vector<uint8_t>& output, size_t value) {
    output.push_back(static_cast<uint8_t>(value));
    output.push_back(static_cast<uint8_t>(value >> 8));
}

void AppendAttribute(std::vector<uint8_t>& output, uint8_t id, const std::string& value) {
    output.push_back(id);
    Append16(output, value.size());
    output.insert(output.end(), value.begin(), value.end());
}

std::vector<uint8_t> Response(uint32_t uid, const std::string& app, const std::string& title,
                              const std::string& subtitle, const std::string& message) {
    std::vector<uint8_t> output = {
        0,
        static_cast<uint8_t>(uid),
        static_cast<uint8_t>(uid >> 8),
        static_cast<uint8_t>(uid >> 16),
        static_cast<uint8_t>(uid >> 24),
    };
    AppendAttribute(output, 0, app);
    AppendAttribute(output, 1, title);
    AppendAttribute(output, 2, subtitle);
    AppendAttribute(output, 3, message);
    return output;
}

void TestFragmentedUtf8AndEmptyFields() {
    const std::string title = "\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x94\x94";
    const auto response = Response(0x78563412, "com.example.app", title, "", "body");
    AncsAttributeParser parser;
    for (size_t index = 0; index < response.size(); ++index) {
        const auto status = parser.Append(&response[index], 1);
        assert(status == (index + 1 == response.size() ? AncsAttributeParser::Status::kComplete
                                                       : AncsAttributeParser::Status::kNeedMore));
    }
    assert(parser.result().uid == 0x78563412);
    assert(parser.result().title == title);
    assert(parser.result().subtitle.empty());
    assert(parser.result().message == "body");
    assert(!parser.result().truncated);
}

void TestTruncationAndInvalidUtf8() {
    std::string long_title(300, 't');
    std::string invalid_message = "ok";
    invalid_message.push_back(static_cast<char>(0xff));
    const auto response = Response(7, "app", long_title, "", invalid_message);
    AncsAttributeParser parser;
    assert(parser.Append(response.data(), response.size()) ==
           AncsAttributeParser::Status::kComplete);
    assert(parser.result().title.size() == 256);
    assert(parser.result().truncated);
    assert(parser.result().message == "ok");

    parser.Reset();
    invalid_message = "a";
    invalid_message.push_back(static_cast<char>(0xff));
    invalid_message += "b";
    const auto invalid_only = Response(8, "app", "title", "", invalid_message);
    assert(parser.Append(invalid_only.data(), invalid_only.size()) ==
           AncsAttributeParser::Status::kComplete);
    assert(parser.result().message == "ab");
    assert(parser.result().truncated);
}

void TestMalformedAndOversizedResponses() {
    auto duplicate = Response(9, "app", "title", "", "body");
    duplicate[5 + 3 + 3] = 0;
    AncsAttributeParser parser;
    assert(parser.Append(duplicate.data(), duplicate.size()) ==
           AncsAttributeParser::Status::kInvalid);

    parser.Reset();
    std::vector<uint8_t> oversized(AncsAttributeParser::kMaxResponseBytes + 1, 0);
    assert(parser.Append(oversized.data(), oversized.size()) ==
           AncsAttributeParser::Status::kTooLarge);

    parser.Reset();
    const auto complete = Response(10, "app", "title", "", "after timeout");
    assert(parser.Append(complete.data(), 4) == AncsAttributeParser::Status::kNeedMore);
    parser.Reset();
    assert(parser.Append(complete.data(), complete.size()) ==
           AncsAttributeParser::Status::kComplete);

    parser.Reset();
    const auto wrong_uid = Response(11, "app", "title", "", "wrong uid");
    assert(parser.Append(wrong_uid.data(), wrong_uid.size()) ==
           AncsAttributeParser::Status::kComplete);
    assert(parser.result().uid != 10);
}

void TestNotificationRecordsAndPreExistingFilter() {
    const uint8_t added[] = {0, 0, 4, 1, 0x78, 0x56, 0x34, 0x12};
    const uint8_t modified[] = {1, 0, 6, 1, 7, 0, 0, 0};
    const uint8_t removed[] = {2, 0, 6, 0, 7, 0, 0, 0};
    const uint8_t invalid[] = {3, 0, 0, 0, 0, 0, 0, 0};
    AncsNotification notification;
    assert(ParseAncsNotificationRecord(added, sizeof(added), notification));
    assert(notification.event == AncsEventType::kAdded);
    assert(notification.category_id == 4);
    assert(notification.uid == 0x12345678);
    assert(ParseAncsNotificationRecord(modified, sizeof(modified), notification));
    assert(notification.event == AncsEventType::kModified);
    assert(ParseAncsNotificationRecord(removed, sizeof(removed), notification));
    assert(notification.event == AncsEventType::kRemoved);
    assert(!ParseAncsNotificationRecord(invalid, sizeof(invalid), notification));
    assert(!ParseAncsNotificationRecord(added, sizeof(added) - 1, notification));

    AncsPreExistingFilter filter(2);
    AncsNotification historic;
    historic.uid = 7;
    historic.event_flags = 1 << 2;
    assert(filter.ShouldIgnore(historic));
    historic.event_flags = 0;
    historic.event = AncsEventType::kModified;
    assert(filter.ShouldIgnore(historic));
    historic.event = AncsEventType::kRemoved;
    assert(filter.ShouldIgnore(historic));
    historic.event = AncsEventType::kAdded;
    assert(!filter.ShouldIgnore(historic));
}

void TestDeliveryPolicy() {
    assert(ClassifyAncsHttpStatus(200) == AncsDeliveryDecision::kSuccess);
    assert(ClassifyAncsHttpStatus(299) == AncsDeliveryDecision::kSuccess);
    assert(ClassifyAncsHttpStatus(0) == AncsDeliveryDecision::kRetry);
    assert(ClassifyAncsHttpStatus(408) == AncsDeliveryDecision::kRetry);
    assert(ClassifyAncsHttpStatus(429) == AncsDeliveryDecision::kRetry);
    assert(ClassifyAncsHttpStatus(503) == AncsDeliveryDecision::kRetry);
    assert(ClassifyAncsHttpStatus(600) == AncsDeliveryDecision::kDrop);
    assert(ClassifyAncsHttpStatus(400) == AncsDeliveryDecision::kDrop);
    assert(ClassifyAncsHttpStatus(404) == AncsDeliveryDecision::kDrop);
    const uint32_t expected[] = {1, 2, 4, 8, 16, 30, 30};
    for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); ++index) {
        assert(AncsRetryDelaySeconds(index) == expected[index]);
    }

    std::deque<int> queue;
    for (int value = 1; value <= 20; ++value) {
        assert(!PushAncsBoundedFifo(queue, 20, value));
    }
    assert(PushAncsBoundedFifo(queue, 20, 21));
    assert(queue.size() == 20);
    assert(queue.front() == 2);
    assert(queue.back() == 21);
}

void TestWebhookJsonEscapingAndIdentity() {
    AncsNotification notification;
    notification.event = AncsEventType::kModified;
    notification.session_id = "session";
    notification.sequence = 42;
    notification.uid = 123;
    notification.category_id = 4;
    notification.event_flags = (1 << 0) | (1 << 4);
    notification.app_identifier = "com.example.app";
    notification.title = "quote \" slash \\ newline\n";
    notification.subtitle = "";
    notification.message = "\xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x94\x94";
    notification.truncated = true;
    const auto payload = SerializeAncsWebhook(
        notification, {.id = "board-id", .mac = "aa:bb:cc:dd:ee:ff", .board = "lichuang-dev-ancs"});
    assert(payload.event_id == "board-id:session:42");
    cJSON* root = cJSON_Parse(payload.json.c_str());
    assert(root != nullptr);
    assert(cJSON_GetObjectItem(root, "schema_version")->valueint == 1);
    assert(std::string(cJSON_GetObjectItem(root, "event_id")->valuestring) == payload.event_id);
    cJSON* ancs = cJSON_GetObjectItem(root, "ancs");
    assert(std::string(cJSON_GetObjectItem(ancs, "event")->valuestring) == "modified");
    assert(std::string(cJSON_GetObjectItem(ancs, "title")->valuestring) == notification.title);
    assert(std::string(cJSON_GetObjectItem(ancs, "message")->valuestring) == notification.message);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(ancs, "truncated")));
    cJSON* flags = cJSON_GetObjectItem(ancs, "flags");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(flags, "silent")));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(flags, "negative_action")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(flags, "important")));
    cJSON_Delete(root);
}

}  // namespace

int main() {
    TestFragmentedUtf8AndEmptyFields();
    TestTruncationAndInvalidUtf8();
    TestMalformedAndOversizedResponses();
    TestNotificationRecordsAndPreExistingFilter();
    TestDeliveryPolicy();
    TestWebhookJsonEscapingAndIdentity();
    return 0;
}
