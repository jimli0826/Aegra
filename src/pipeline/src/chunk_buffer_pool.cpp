#include "chunk_buffer_pool.h"

#include <chrono>
#include <new>
#include <utility>

namespace aegra::pipeline::detail {
namespace {

[[nodiscard]] std::uint64_t elapsed_microseconds(
    const std::chrono::steady_clock::time_point start) noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
}

[[nodiscard]] base::Error cancelled_error() {
    return base::Error{base::ErrorCode::kCancelled, "pipeline buffer wait cancelled"};
}

[[nodiscard]] base::Error allocation_error() {
    return base::Error{base::ErrorCode::kInsufficientSpace,
                       "pipeline chunk buffer allocation failed"};
}

} // namespace

ChunkBufferPool::ChunkBufferPool(const std::size_t maximum_buffers)
    : maximum_buffers_(maximum_buffers) {
    available_.reserve(maximum_buffers_);
}

base::Result<AcquiredChunkBuffer>
ChunkBufferPool::acquire(const std::size_t required_size,
                         const base::CancellationToken& cancellation) {
    std::vector<std::byte> payload;
    std::uint64_t wait_microseconds = 0;
    {
        std::unique_lock lock(mutex_);
        if (available_.empty() && allocated_buffers_ >= maximum_buffers_) {
            const auto wait_start = std::chrono::steady_clock::now();
            const auto ready = available_changed_.wait(
                lock, cancellation, [this] { return !available_.empty(); });
            wait_microseconds = elapsed_microseconds(wait_start);
            if (!ready) {
                return base::Result<AcquiredChunkBuffer>::failure(cancelled_error());
            }
        }
        if (available_.empty()) {
            ++allocated_buffers_;
        } else {
            payload = std::move(available_.back());
            available_.pop_back();
        }
    }

    const auto allocation_start = std::chrono::steady_clock::now();
    try {
        payload.resize(required_size);
    } catch (const std::bad_alloc&) {
        const std::scoped_lock lock(mutex_);
        --allocated_buffers_;
        available_changed_.notify_one();
        return base::Result<AcquiredChunkBuffer>::failure(allocation_error());
    }
    const auto allocate_microseconds = elapsed_microseconds(allocation_start);
    return base::Result<AcquiredChunkBuffer>::success(
        {std::move(payload), wait_microseconds, allocate_microseconds});
}

void ChunkBufferPool::release(std::vector<std::byte> payload) noexcept {
    const std::scoped_lock lock(mutex_);
    available_.push_back(std::move(payload));
    available_changed_.notify_one();
}

} // namespace aegra::pipeline::detail
