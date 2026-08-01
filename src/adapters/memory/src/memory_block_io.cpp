#include "aegra/adapters/memory/memory_block_io.h"

#include <algorithm>
#include <utility>

namespace aegra::adapters::memory {
namespace {

base::Error cancelled_error() {
    return base::Error{base::ErrorCode::kCancelled, "operation cancelled"};
}

base::Error io_error() {
    return base::Error{base::ErrorCode::kIoFailure, "injected memory I/O failure"};
}

} // namespace

MemoryBlockSource::MemoryBlockSource(std::vector<std::byte> data, MemoryBlockSourceOptions options)
    : data_(std::move(data)), options_(options) {}

std::uint64_t MemoryBlockSource::size_bytes() const noexcept {
    return static_cast<std::uint64_t>(data_.size());
}

base::Result<std::size_t> MemoryBlockSource::read(const std::uint64_t offset,
                                                  const std::span<std::byte> destination,
                                                  const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::size_t>::failure(cancelled_error());
    }
    if (offset > data_.size()) {
        return base::Result<std::size_t>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "read offset is out of range"});
    }
    if (destination.empty() || offset == data_.size()) {
        return base::Result<std::size_t>::success(0);
    }
    if (options_.fail_at_offset && offset >= *options_.fail_at_offset) {
        return base::Result<std::size_t>::failure(io_error());
    }

    const auto available = data_.size() - static_cast<std::size_t>(offset);
    auto count = (std::min)({destination.size(), available, options_.max_read_size});
    if (options_.fail_at_offset && offset + count > *options_.fail_at_offset) {
        count = static_cast<std::size_t>(*options_.fail_at_offset - offset);
    }
    if (count == 0) {
        return base::Result<std::size_t>::failure(io_error());
    }
    const auto source =
        std::span<const std::byte>(data_).subspan(static_cast<std::size_t>(offset), count);
    std::ranges::copy(source, destination.begin());
    return base::Result<std::size_t>::success(count);
}

MemoryBlockSink::MemoryBlockSink(const std::size_t capacity, const MemoryBlockSinkOptions options)
    : data_(capacity), options_(options) {}

std::uint64_t MemoryBlockSink::capacity_bytes() const noexcept {
    return static_cast<std::uint64_t>(data_.size());
}

base::Result<void> MemoryBlockSink::write(const std::uint64_t offset,
                                          const std::span<const std::byte> source,
                                          const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(cancelled_error());
    }
    if (offset > data_.size() || source.size() > data_.size() - offset) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInsufficientSpace,
            "memory block sink capacity is insufficient",
        });
    }
    if (source.empty()) {
        return base::Result<void>::success();
    }
    if (options_.fail_at_offset &&
        (offset >= *options_.fail_at_offset || source.size() > *options_.fail_at_offset - offset)) {
        return base::Result<void>::failure(io_error());
    }

    const std::scoped_lock lock(mutex_);
    auto destination =
        std::span<std::byte>(data_).subspan(static_cast<std::size_t>(offset), source.size());
    std::ranges::copy(source, destination.begin());
    return base::Result<void>::success();
}

base::Result<void> MemoryBlockSink::flush(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(cancelled_error());
    }
    if (options_.fail_flush) {
        return base::Result<void>::failure(io_error());
    }
    const std::scoped_lock lock(mutex_);
    ++flush_count_;
    return base::Result<void>::success();
}

std::vector<std::byte> MemoryBlockSink::snapshot() const {
    const std::scoped_lock lock(mutex_);
    return data_;
}

std::size_t MemoryBlockSink::flush_count() const {
    const std::scoped_lock lock(mutex_);
    return flush_count_;
}

} // namespace aegra::adapters::memory
