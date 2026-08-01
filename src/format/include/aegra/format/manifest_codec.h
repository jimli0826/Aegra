#pragma once

#include "aegra/base/result.h"
#include "aegra/format/manifest.h"

#include <cstddef>
#include <span>
#include <vector>

namespace aegra::format {

[[nodiscard]] base::Result<std::vector<std::byte>> encode_manifest_cbor(const Manifest& manifest);
[[nodiscard]] base::Result<Manifest> decode_manifest_cbor(std::span<const std::byte> encoded);

} // namespace aegra::format
