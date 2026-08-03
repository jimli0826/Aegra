#include "application_ids.h"

#include "aegra/base/error.h"
#include "aegra/ports/random.h"

#include <algorithm>
#include <array>
#include <limits>

namespace aegra::application::detail {
namespace {

[[nodiscard]] bool valid_stable_character(const unsigned char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.' ||
           value == '_' || value == '-' || value == ':';
}

} // namespace

base::Result<void> require_idempotency_key(const std::string_view idempotency_key) {
    if (idempotency_key.empty() || idempotency_key.size() > 128 ||
        !std::ranges::all_of(idempotency_key, [](const unsigned char character) {
            return valid_stable_character(character);
        })) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "idempotency key is invalid"});
    }
    return base::Result<void>::success();
}

base::Result<std::string> make_random_id(const std::string_view prefix,
                                         ports::IRandomSource& random,
                                         const base::CancellationToken& cancellation) {
    std::array<std::byte, 16> bytes{};
    if (auto filled = random.fill(bytes, cancellation); !filled) {
        return base::Result<std::string>::failure(filled.error());
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string id(prefix);
    id.reserve(prefix.size() + bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        id.push_back(kHex[value >> 4U]);
        id.push_back(kHex[value & 0x0FU]);
    }
    return base::Result<std::string>::success(std::move(id));
}

bool is_source_selectable(const ports::SourceInventoryRecord& record) noexcept {
    return record.availability == contracts::SourceAvailability::kAvailable && !record.is_system &&
           !record.is_read_only && record.capacity_bytes > 0 &&
           record.capacity_bytes <=
               static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
}

} // namespace aegra::application::detail
