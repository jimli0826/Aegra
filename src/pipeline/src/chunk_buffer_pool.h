#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace aegra::pipeline::detail {

struct AcquiredChunkBuffer final {
    std::vector<std::byte> payload;
    std::uint64_t wait_microseconds{0};
    std::uint64_t allocate_microseconds{0};
};

class ChunkBufferPool final {
  public:
    explicit ChunkBufferPool(std::size_t maximum_buffers);

    [[nodiscard]] base::Result<AcquiredChunkBuffer>
    acquire(std::size_t required_size, const base::CancellationToken& cancellation);
    void release(std::vector<std::byte> payload) noexcept;

    ChunkBufferPool(const ChunkBufferPool&) = delete;
    ChunkBufferPool& operator=(const ChunkBufferPool&) = delete;
    ChunkBufferPool(ChunkBufferPool&&) = delete;
    ChunkBufferPool& operator=(ChunkBufferPool&&) = delete;

  private:
    const std::size_t maximum_buffers_;
    std::mutex mutex_;
    std::condition_variable_any available_changed_;
    // Reserved to maximum_buffers_ at construction, so the noexcept consumer release cannot
    // allocate. One producer acquires buffers and one consumer returns them.
    std::vector<std::vector<std::byte>> available_;
    std::size_t allocated_buffers_{0};
};

} // namespace aegra::pipeline::detail
