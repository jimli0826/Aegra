#include "cli_ids.h"

#include "aegra/base/error.h"

#include <array>
#include <cstddef>

namespace aegra::apps::cli {
namespace {

constexpr char kHex[] = "0123456789abcdef";

[[nodiscard]] std::string hex_uuid(const std::array<std::byte, 16>& bytes) {
    std::string id;
    id.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            id.push_back('-');
        }
        const auto value = std::to_integer<unsigned>(bytes[index]);
        id.push_back(kHex[value >> 4U]);
        id.push_back(kHex[value & 0x0FU]);
    }
    return id;
}

} // namespace

base::Result<std::string> make_random_uuid(ports::IRandomSource& random,
                                           const base::CancellationToken& cancellation) {
    std::array<std::byte, 16> bytes{};
    if (auto filled = random.fill(bytes, cancellation); !filled) {
        return base::Result<std::string>::failure(filled.error());
    }
    bytes[6] = static_cast<std::byte>((std::to_integer<unsigned>(bytes[6]) & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::byte>((std::to_integer<unsigned>(bytes[8]) & 0x3FU) | 0x80U);
    return base::Result<std::string>::success(hex_uuid(bytes));
}

base::Result<std::string> make_idempotency_key(const std::string_view prefix,
                                               ports::IRandomSource& random,
                                               const base::CancellationToken& cancellation) {
    auto uuid = make_random_uuid(random, cancellation);
    if (!uuid) {
        return uuid;
    }
    std::string key;
    key.reserve(prefix.size() + 1 + uuid.value().size());
    key.append(prefix);
    key.push_back(':');
    key.append(uuid.value());
    if (key.size() > 128) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInternal, "idempotency key exceeded 128 bytes"});
    }
    return base::Result<std::string>::success(std::move(key));
}

} // namespace aegra::apps::cli
