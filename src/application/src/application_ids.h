#pragma once

#include "aegra/base/result.h"
#include "aegra/ports/random.h"
#include "aegra/ports/source_inventory.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace aegra::application::detail {

[[nodiscard]] base::Result<void> require_idempotency_key(std::string_view idempotency_key);
[[nodiscard]] base::Result<std::string> make_random_id(std::string_view prefix,
                                                       ports::IRandomSource& random,
                                                       const base::CancellationToken& cancellation);
// Lowercase RFC 4122 text UUID (version 4 bits set).
[[nodiscard]] base::Result<std::string>
make_random_uuid(ports::IRandomSource& random, const base::CancellationToken& cancellation);
[[nodiscard]] bool is_source_selectable(const ports::SourceInventoryRecord& record) noexcept;

} // namespace aegra::application::detail
