#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class AncsAttributeParser {
public:
    static constexpr size_t kMaxResponseBytes = 2048;

    enum class Status {
        kNeedMore,
        kComplete,
        kInvalid,
        kTooLarge,
    };

    struct Result {
        uint32_t uid = 0;
        std::string app_identifier;
        std::string title;
        std::string subtitle;
        std::string message;
        bool truncated = false;
    };

    void Reset();
    Status Append(const uint8_t* data, size_t size);
    const Result& result() const { return result_; }

private:
    Status Parse();
    static std::string SanitizeUtf8(const uint8_t* data, size_t size);

    std::vector<uint8_t> buffer_;
    Result result_;
};
