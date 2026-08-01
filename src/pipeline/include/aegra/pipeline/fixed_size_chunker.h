#pragma once

#include "aegra/base/result.h"

#include <cstdint>
#include <optional>

namespace aegra::pipeline {

struct ChunkRange final {
    std::uint64_t chunk_index{0};
    std::uint64_t logical_offset{0};
    std::uint64_t logical_size{0};
};

class FixedSizeChunker final {
  public:
    [[nodiscard]] static base::Result<FixedSizeChunker> create(std::uint64_t chunk_size_bytes);

    [[nodiscard]] base::Result<std::optional<ChunkRange>> next(std::uint64_t total_size_bytes,
                                                               std::uint64_t logical_offset,
                                                               std::uint64_t chunk_index) const;

    [[nodiscard]] std::uint64_t chunk_size_bytes() const noexcept;

  private:
    explicit FixedSizeChunker(std::uint64_t chunk_size_bytes) noexcept;

    std::uint64_t chunk_size_bytes_;
};

} // namespace aegra::pipeline
