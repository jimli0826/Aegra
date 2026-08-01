#include "aegra/pipeline/fixed_size_chunker.h"

#include <algorithm>

namespace aegra::pipeline {

FixedSizeChunker::FixedSizeChunker(const std::uint64_t chunk_size_bytes) noexcept
    : chunk_size_bytes_(chunk_size_bytes) {}

base::Result<FixedSizeChunker> FixedSizeChunker::create(const std::uint64_t chunk_size_bytes) {
    if (chunk_size_bytes == 0) {
        return base::Result<FixedSizeChunker>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "chunk_size_bytes must be greater than zero",
        });
    }
    return base::Result<FixedSizeChunker>::success(FixedSizeChunker(chunk_size_bytes));
}

base::Result<std::optional<ChunkRange>>
FixedSizeChunker::next(const std::uint64_t total_size_bytes, const std::uint64_t logical_offset,
                       const std::uint64_t chunk_index) const {
    if (logical_offset > total_size_bytes) {
        return base::Result<std::optional<ChunkRange>>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "logical offset exceeds source size",
        });
    }
    if (logical_offset == total_size_bytes) {
        return base::Result<std::optional<ChunkRange>>::success(std::nullopt);
    }
    const auto remaining = total_size_bytes - logical_offset;
    const auto logical_size = (std::min)(remaining, chunk_size_bytes_);
    return base::Result<std::optional<ChunkRange>>::success(
        ChunkRange{chunk_index, logical_offset, logical_size});
}

std::uint64_t FixedSizeChunker::chunk_size_bytes() const noexcept { return chunk_size_bytes_; }

} // namespace aegra::pipeline
