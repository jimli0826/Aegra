#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/backup_session.h"

#include "owned_chunk_buffer.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace aegra::pipeline::detail {

struct QueuedChunk final {
    ports::ChunkDescriptor descriptor;
    OwnedChunkBuffer payload;
};

class BoundedChunkQueue final {
  public:
    explicit BoundedChunkQueue(std::size_t byte_budget);

    [[nodiscard]] base::Result<void> push(QueuedChunk chunk,
                                          const base::CancellationToken& cancellation);
    [[nodiscard]] base::Result<std::optional<QueuedChunk>>
    pop(const base::CancellationToken& cancellation);

    void close() noexcept;
    void fail(base::Error error) noexcept;
    [[nodiscard]] std::size_t peak_buffered_bytes() const noexcept;

  private:
    [[nodiscard]] bool can_push(std::size_t bytes) const noexcept;

    const std::size_t byte_budget_;
    mutable std::mutex mutex_;
    std::condition_variable_any state_changed_;
    std::deque<QueuedChunk> chunks_;
    std::optional<base::Error> failure_;
    std::size_t buffered_bytes_{0};
    std::size_t peak_buffered_bytes_{0};
    bool closed_{false};
};

} // namespace aegra::pipeline::detail
