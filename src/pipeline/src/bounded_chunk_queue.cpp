#include "bounded_chunk_queue.h"

#include <algorithm>
#include <utility>

namespace aegra::pipeline::detail {
namespace {

base::Error cancelled_error() {
    return base::Error{base::ErrorCode::kCancelled, "pipeline cancelled"};
}

} // namespace

BoundedChunkQueue::BoundedChunkQueue(const std::size_t byte_budget) : byte_budget_(byte_budget) {}

bool BoundedChunkQueue::can_push(const std::size_t bytes) const noexcept {
    return bytes <= byte_budget_ && bytes <= byte_budget_ - buffered_bytes_;
}

base::Result<void> BoundedChunkQueue::push(QueuedChunk chunk,
                                           const base::CancellationToken& cancellation) {
    const auto bytes = chunk.payload.bytes().size();
    if (bytes > byte_budget_) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInsufficientSpace,
            "chunk exceeds pipeline memory budget",
        });
    }

    std::unique_lock lock(mutex_);
    const auto ready = state_changed_.wait(lock, cancellation, [this, bytes] {
        return failure_.has_value() || closed_ || can_push(bytes);
    });
    if (!ready) {
        return base::Result<void>::failure(cancelled_error());
    }
    if (failure_) {
        return base::Result<void>::failure(*failure_);
    }
    if (closed_) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kConflict, "pipeline queue is closed"});
    }

    buffered_bytes_ += bytes;
    peak_buffered_bytes_ = (std::max)(peak_buffered_bytes_, buffered_bytes_);
    chunks_.push_back(std::move(chunk));
    state_changed_.notify_all();
    return base::Result<void>::success();
}

base::Result<std::optional<QueuedChunk>>
BoundedChunkQueue::pop(const base::CancellationToken& cancellation) {
    std::unique_lock lock(mutex_);
    const auto ready = state_changed_.wait(
        lock, cancellation, [this] { return failure_.has_value() || closed_ || !chunks_.empty(); });
    if (!ready) {
        return base::Result<std::optional<QueuedChunk>>::failure(cancelled_error());
    }
    if (failure_) {
        return base::Result<std::optional<QueuedChunk>>::failure(*failure_);
    }
    if (chunks_.empty()) {
        return base::Result<std::optional<QueuedChunk>>::success(std::nullopt);
    }

    auto chunk = std::move(chunks_.front());
    chunks_.pop_front();
    buffered_bytes_ -= chunk.payload.bytes().size();
    state_changed_.notify_all();
    return base::Result<std::optional<QueuedChunk>>::success(std::move(chunk));
}

void BoundedChunkQueue::close() noexcept {
    const std::scoped_lock lock(mutex_);
    closed_ = true;
    state_changed_.notify_all();
}

void BoundedChunkQueue::fail(base::Error error) noexcept {
    const std::scoped_lock lock(mutex_);
    if (!failure_) {
        failure_ = std::move(error);
    }
    chunks_.clear();
    buffered_bytes_ = 0;
    closed_ = true;
    state_changed_.notify_all();
}

std::size_t BoundedChunkQueue::peak_buffered_bytes() const noexcept {
    const std::scoped_lock lock(mutex_);
    return peak_buffered_bytes_;
}

} // namespace aegra::pipeline::detail
