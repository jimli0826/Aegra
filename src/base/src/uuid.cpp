#include "aegra/base/uuid.h"

#include "aegra/base/error.h"

namespace aegra::base {
namespace {

[[nodiscard]] int hex_value(const char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

} // namespace

bool is_canonical_uuid(const std::string_view value) noexcept {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
        value[23] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        if (hex_value(value[index]) < 0) {
            return false;
        }
    }
    const bool valid_version = value[14] >= '1' && value[14] <= '5';
    const bool valid_variant =
        value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b';
    return valid_version && valid_variant;
}

Result<UuidBytes> parse_uuid(const std::string_view value) {
    if (!is_canonical_uuid(value)) {
        return Result<UuidBytes>::failure(
            {ErrorCode::kInvalidArgument, "UUID is not canonical RFC 4122 text"});
    }
    UuidBytes result{};
    std::size_t source = 0;
    for (std::size_t target = 0; target < result.size(); ++target) {
        if (source == 8 || source == 13 || source == 18 || source == 23) {
            ++source;
        }
        const auto high = static_cast<unsigned>(hex_value(value[source++]));
        const auto low = static_cast<unsigned>(hex_value(value[source++]));
        result[target] = static_cast<std::byte>((high << 4U) | low);
    }
    return Result<UuidBytes>::success(result);
}

std::string format_uuid(const UuidBytes& value) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result.push_back('-');
        }
        const auto byte = std::to_integer<unsigned>(value[index]);
        result.push_back(kHex[byte >> 4U]);
        result.push_back(kHex[byte & 0x0FU]);
    }
    return result;
}

} // namespace aegra::base
