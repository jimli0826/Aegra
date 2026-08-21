#pragma once

#include "aegra/base/result.h"
#include "aegra/ntfs_resize/shrink_plan.h"

#include <cstddef>
#include <span>
#include <vector>

namespace aegra::ntfs_resize {

/// Encodes an immutable ShrinkPlan to little-endian AGSP v1 bytes.
/// Recomputes the canonical payload digest and rejects mismatch with plan_payload_digest.
[[nodiscard]] base::Result<std::vector<std::byte>> encode_shrink_plan(const ShrinkPlan& plan);

/// Decodes and validates AGSP bytes. Rejects truncated input, unknown version, bad CRC/digest,
/// duplicate sections, invalid half-open ranges, and overlapping relocation targets.
[[nodiscard]] base::Result<ShrinkPlan> decode_shrink_plan(std::span<const std::byte> bytes);

} // namespace aegra::ntfs_resize
