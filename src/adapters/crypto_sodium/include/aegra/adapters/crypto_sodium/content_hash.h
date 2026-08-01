#pragma once

#include "aegra/base/result.h"

#include <array>
#include <cstddef>
#include <span>

namespace aegra::adapters::crypto_sodium {

inline constexpr std::size_t kSha256DigestSize = 32;
using Sha256Digest = std::array<std::byte, kSha256DigestSize>;

[[nodiscard]] base::Result<Sha256Digest> sha256(std::span<const std::byte> input);

} // namespace aegra::adapters::crypto_sodium
