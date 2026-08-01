#pragma once

#include "aegra/base/result.h"

#include <cstddef>
#include <span>
#include <vector>

namespace aegra::adapters::compression_zstd {

inline constexpr int kDefaultCompressionLevel = 3;

[[nodiscard]] base::Result<std::vector<std::byte>>
compress(std::span<const std::byte> input, int compression_level = kDefaultCompressionLevel);

[[nodiscard]] base::Result<std::vector<std::byte>> decompress(std::span<const std::byte> input,
                                                              std::size_t expected_size,
                                                              std::size_t maximum_output_size);

} // namespace aegra::adapters::compression_zstd
