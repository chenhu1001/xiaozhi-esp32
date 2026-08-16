#include "ancs_attribute_parser.h"

#include <algorithm>

namespace {

constexpr uint8_t kCommandGetNotificationAttributes = 0;
constexpr size_t kHeaderSize = 5;
constexpr std::array<size_t, 4> kAttributeLimits = {
    0,
    256,
    256,
    1024,
};

uint16_t ReadLittleEndian16(const uint8_t* value) {
    return static_cast<uint16_t>(value[0]) | (static_cast<uint16_t>(value[1]) << 8);
}

uint32_t ReadLittleEndian32(const uint8_t* value) {
    return static_cast<uint32_t>(value[0]) | (static_cast<uint32_t>(value[1]) << 8) |
           (static_cast<uint32_t>(value[2]) << 16) | (static_cast<uint32_t>(value[3]) << 24);
}

bool IsContinuation(uint8_t value) { return (value & 0xc0) == 0x80; }

}  // namespace

void AncsAttributeParser::Reset() {
    buffer_.clear();
    result_ = {};
}

AncsAttributeParser::Status AncsAttributeParser::Append(const uint8_t* data, size_t size) {
    if (size > kMaxResponseBytes - buffer_.size()) {
        return Status::kTooLarge;
    }
    if (size == 0) {
        return Parse();
    }
    buffer_.insert(buffer_.end(), data, data + size);
    return Parse();
}

AncsAttributeParser::Status AncsAttributeParser::Parse() {
    if (buffer_.size() < kHeaderSize) {
        return Status::kNeedMore;
    }
    if (buffer_[0] != kCommandGetNotificationAttributes) {
        return Status::kInvalid;
    }

    Result parsed;
    parsed.uid = ReadLittleEndian32(buffer_.data() + 1);
    std::array<bool, 4> seen = {{false, false, false, false}};
    size_t offset = kHeaderSize;

    while (offset < buffer_.size()) {
        if (buffer_.size() - offset < 3) {
            return Status::kNeedMore;
        }
        const uint8_t attribute_id = buffer_[offset];
        const uint16_t attribute_size = ReadLittleEndian16(buffer_.data() + offset + 1);
        offset += 3;
        if (attribute_id >= seen.size() || seen[attribute_id]) {
            return Status::kInvalid;
        }
        if (buffer_.size() - offset < attribute_size) {
            return Status::kNeedMore;
        }

        size_t kept_size = attribute_size;
        if (kAttributeLimits[attribute_id] != 0) {
            kept_size = std::min(kept_size, kAttributeLimits[attribute_id]);
            if (attribute_size >= kAttributeLimits[attribute_id]) {
                parsed.truncated = true;
            }
        }
        std::string value = SanitizeUtf8(buffer_.data() + offset, kept_size);
        if (value.size() != kept_size) {
            parsed.truncated = true;
        }
        switch (attribute_id) {
            case 0:
                parsed.app_identifier = std::move(value);
                break;
            case 1:
                parsed.title = std::move(value);
                break;
            case 2:
                parsed.subtitle = std::move(value);
                break;
            case 3:
                parsed.message = std::move(value);
                break;
        }
        seen[attribute_id] = true;
        offset += attribute_size;
    }

    if (!std::all_of(seen.begin(), seen.end(), [](bool value) { return value; })) {
        return Status::kNeedMore;
    }
    result_ = std::move(parsed);
    return Status::kComplete;
}

std::string AncsAttributeParser::SanitizeUtf8(const uint8_t* data, size_t size) {
    std::string output;
    output.reserve(size);
    size_t index = 0;
    while (index < size) {
        const uint8_t lead = data[index];
        size_t sequence_size = 0;
        uint32_t codepoint = 0;
        if (lead <= 0x7f) {
            sequence_size = 1;
            codepoint = lead;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            sequence_size = 2;
            codepoint = lead & 0x1f;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            sequence_size = 3;
            codepoint = lead & 0x0f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            sequence_size = 4;
            codepoint = lead & 0x07;
        }

        bool valid = sequence_size != 0 && index + sequence_size <= size;
        for (size_t part = 1; valid && part < sequence_size; ++part) {
            valid = IsContinuation(data[index + part]);
            if (valid) {
                codepoint = (codepoint << 6) | (data[index + part] & 0x3f);
            }
        }
        if (valid) {
            valid = (sequence_size != 3 || codepoint >= 0x800) &&
                    (sequence_size != 4 || codepoint >= 0x10000) && codepoint <= 0x10ffff &&
                    !(codepoint >= 0xd800 && codepoint <= 0xdfff);
        }

        if (valid) {
            output.append(reinterpret_cast<const char*>(data + index), sequence_size);
            index += sequence_size;
        } else {
            ++index;
        }
    }
    return output;
}
