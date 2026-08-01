#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aegra::base {

enum class ErrorCode : std::uint32_t {
    kNone = 0,
    kInvalidArgument = 1,
    kUnsupportedVersion = 2,
    kCancelled = 3,
    kIoFailure = 4,
    kCorruptData = 5,
    kNotFound = 6,
    kConflict = 7,
    kUnauthorized = 8,
    kInternal = 9,
};

struct Error final {
    ErrorCode code{ErrorCode::kNone};
    std::string message;

    [[nodiscard]] bool is_error() const noexcept;
};

[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

} // namespace aegra::base
