#pragma once

#include "aegra/base/result.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace aegra::base {

using UuidBytes = std::array<std::byte, 16>;

[[nodiscard]] bool is_canonical_uuid(std::string_view value) noexcept;
[[nodiscard]] Result<UuidBytes> parse_uuid(std::string_view value);
[[nodiscard]] std::string format_uuid(const UuidBytes& value);

} // namespace aegra::base
