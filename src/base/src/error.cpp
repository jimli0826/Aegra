#include "aegra/base/error.h"

namespace aegra::base {

bool Error::is_error() const noexcept {
    return code != ErrorCode::kNone;
}

std::string_view error_code_name(const ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::kNone:
        return "none";
    case ErrorCode::kInvalidArgument:
        return "invalid_argument";
    case ErrorCode::kUnsupportedVersion:
        return "unsupported_version";
    case ErrorCode::kCancelled:
        return "cancelled";
    case ErrorCode::kIoFailure:
        return "io_failure";
    case ErrorCode::kCorruptData:
        return "corrupt_data";
    case ErrorCode::kNotFound:
        return "not_found";
    case ErrorCode::kConflict:
        return "conflict";
    case ErrorCode::kUnauthorized:
        return "unauthorized";
    case ErrorCode::kInternal:
        return "internal";
    }
    return "unknown";
}

} // namespace aegra::base
