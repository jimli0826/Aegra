#include "aegra/ntfs_resize/composite_ntfs_block_device.h"

#include "block_device_byte_io.h"
#include "sparse_overlay_index.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace aegra::ntfs_resize {
namespace {

[[nodiscard]] base::Result<void>
require_range(const std::uint64_t offset, const std::size_t size, const std::uint64_t limit) {
    if (size == 0) {
        return base::Result<void>::success();
    }
    if (offset > limit || size > limit - offset) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "ntfs_resize.composite_range_invalid"});
    }
    return base::Result<void>::success();
}

[[nodiscard]] bool intersects_protected(const std::uint64_t offset, const std::size_t size,
                                        const std::vector<ByteRange>& protected_ranges) noexcept {
    if (size == 0) {
        return false;
    }
    const auto end = offset + size;
    for (const auto& range : protected_ranges) {
        if (range.end <= range.begin) {
            continue;
        }
        if (offset < range.end && range.begin < end) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] const ByteRange*
protected_range_at(const std::uint64_t offset,
                   const std::vector<ByteRange>& protected_ranges) noexcept {
    for (const auto& range : protected_ranges) {
        if (range.begin <= offset && offset < range.end) {
            return &range;
        }
    }
    return nullptr;
}

[[nodiscard]] std::uint64_t
next_protected_boundary(const std::uint64_t offset, const std::uint64_t limit,
                        const std::vector<ByteRange>& protected_ranges) noexcept {
    auto boundary = limit;
    for (const auto& range : protected_ranges) {
        if (range.begin > offset) {
            boundary = (std::min)(boundary, range.begin);
        }
        if (range.end > offset) {
            boundary = (std::min)(boundary, range.end);
        }
    }
    return boundary;
}

[[nodiscard]] base::Result<std::size_t>
read_exact_from_reader(ports::IRandomAccessReader& reader, const std::uint64_t offset,
                       const std::span<std::byte> destination,
                       const base::CancellationToken cancellation) {
    std::size_t total = 0;
    while (total < destination.size()) {
        auto n = reader.read_at(offset + total, destination.subspan(total), cancellation);
        if (!n) {
            return n;
        }
        if (n.value() == 0) {
            return base::Result<std::size_t>::failure(
                {base::ErrorCode::kIoFailure, "ntfs_resize.composite_short_read"});
        }
        total += n.value();
    }
    return base::Result<std::size_t>::success(destination.size());
}

[[nodiscard]] base::Result<std::size_t>
read_exact_from_device(ports::IRandomAccessBlockDevice& device, const std::uint64_t offset,
                       const std::span<std::byte> destination,
                       const base::CancellationToken cancellation) {
    auto n = detail::read_block_device_bytes(device, offset, destination, cancellation);
    if (!n) {
        return n;
    }
    if (n.value() != destination.size()) {
        return base::Result<std::size_t>::failure(
            {base::ErrorCode::kIoFailure, "ntfs_resize.composite_short_read"});
    }
    return n;
}

[[nodiscard]] base::Result<std::size_t>
read_exact_from_scratch(ports::IScratchStore& overlay, const std::uint64_t offset,
                        const std::span<std::byte> destination,
                        const base::CancellationToken cancellation) {
    auto n = overlay.read_at(offset, destination, cancellation);
    if (!n) {
        return n;
    }
    if (n.value() != destination.size()) {
        return base::Result<std::size_t>::failure(
            {base::ErrorCode::kIoFailure, "ntfs_resize.composite_short_read"});
    }
    return n;
}

} // namespace

CompositeNtfsBlockDevice::CompositeNtfsBlockDevice(
    ports::IRandomAccessBlockDevice& target, ports::IRandomAccessReader& source,
    ports::IScratchStore& overlay, const std::uint64_t source_logical_size_bytes,
    const std::uint64_t target_capacity_bytes, std::vector<ByteRange> protected_ranges)
    : target_(&target), source_(&source), overlay_(&overlay),
      source_logical_size_bytes_(source_logical_size_bytes),
      target_capacity_bytes_(target_capacity_bytes),
      protected_ranges_(std::move(protected_ranges)),
      overlay_index_(std::make_unique<detail::SparseOverlayIndex>()) {}

CompositeNtfsBlockDevice::CompositeNtfsBlockDevice(CompositeNtfsBlockDevice&&) noexcept = default;
CompositeNtfsBlockDevice::~CompositeNtfsBlockDevice() = default;

base::Result<CompositeNtfsBlockDevice>
CompositeNtfsBlockDevice::create(const CompositeNtfsBlockDeviceConfig& config) {
    if (config.target == nullptr || config.source == nullptr || config.overlay == nullptr) {
        return base::Result<CompositeNtfsBlockDevice>::failure(
            {base::ErrorCode::kInvalidArgument, "ntfs_resize.composite_missing_dependency"});
    }
    const auto target_capacity = config.target->geometry().capacity_bytes;
    if (config.source_logical_size_bytes == 0 || target_capacity == 0 ||
        config.source_logical_size_bytes <= target_capacity) {
        return base::Result<CompositeNtfsBlockDevice>::failure(
            {base::ErrorCode::kInvalidArgument, "ntfs_resize.composite_size_invalid"});
    }
    if (config.source->size_bytes() < config.source_logical_size_bytes) {
        return base::Result<CompositeNtfsBlockDevice>::failure(
            {base::ErrorCode::kInvalidArgument, "ntfs_resize.composite_source_too_small"});
    }
    if (config.overlay->logical_size_bytes() < config.source_logical_size_bytes) {
        return base::Result<CompositeNtfsBlockDevice>::failure(
            {base::ErrorCode::kInvalidArgument, "ntfs_resize.composite_overlay_too_small"});
    }
    std::vector<ByteRange> protected_ranges(config.protected_ranges.begin(),
                                            config.protected_ranges.end());
    return base::Result<CompositeNtfsBlockDevice>::success(CompositeNtfsBlockDevice(
        *config.target, *config.source, *config.overlay, config.source_logical_size_bytes,
        target_capacity, std::move(protected_ranges)));
}

base::Result<std::size_t>
CompositeNtfsBlockDevice::read_at(const std::uint64_t offset, const std::span<std::byte> destination,
                                  const base::CancellationToken cancellation) {
    if (auto range = require_range(offset, destination.size(), source_logical_size_bytes_); !range) {
        return base::Result<std::size_t>::failure(range.error());
    }
    if (destination.empty()) {
        return base::Result<std::size_t>::success(0);
    }

    std::size_t position = 0;
    while (position < destination.size()) {
        const auto absolute = offset + position;
        const auto page = detail::overlay_page_index(absolute);
        const auto page_begin = page * detail::kOverlayPageSizeBytes;
        const auto page_end = page_begin + detail::kOverlayPageSizeBytes;
        const auto span_end =
            (std::min)(offset + destination.size(), (std::min)(page_end, source_logical_size_bytes_));
        const auto chunk = static_cast<std::size_t>(span_end - absolute);
        auto slice = destination.subspan(position, chunk);

        base::Result<std::size_t> read_result =
            base::Result<std::size_t>::failure({base::ErrorCode::kInternal, "unreachable"});
        if (overlay_index_->contains_page(page)) {
            read_result = read_exact_from_scratch(*overlay_, absolute, slice, cancellation);
        } else if (absolute < target_capacity_bytes_ &&
                   !intersects_protected(absolute, slice.size(), protected_ranges_)) {
            const auto target_chunk =
                static_cast<std::size_t>((std::min)(static_cast<std::uint64_t>(chunk),
                                                    target_capacity_bytes_ - absolute));
            slice = destination.subspan(position, target_chunk);
            read_result = read_exact_from_device(*target_, absolute, slice, cancellation);
        } else {
            // Source/archive supplies bytes for the tail and for protected Boot escrow ranges.
            read_result = read_exact_from_reader(*source_, absolute, slice, cancellation);
        }
        if (!read_result) {
            return read_result;
        }
        position += read_result.value();
    }
    return base::Result<std::size_t>::success(destination.size());
}

base::Result<std::size_t>
CompositeNtfsBlockDevice::read_base_at(const std::uint64_t offset,
                                       const std::span<std::byte> destination,
                                       const base::CancellationToken cancellation) {
    std::size_t done = 0;
    const auto request_end = offset + destination.size();
    while (done < destination.size()) {
        const auto absolute = offset + done;
        auto boundary = next_protected_boundary(absolute, request_end, protected_ranges_);
        if (absolute < target_capacity_bytes_) {
            boundary = (std::min)(boundary, target_capacity_bytes_);
        }
        const auto chunk = static_cast<std::size_t>(boundary - absolute);
        auto slice = destination.subspan(done, chunk);
        base::Result<std::size_t> read =
            base::Result<std::size_t>::failure({base::ErrorCode::kInternal, "unreachable"});
        if (absolute < target_capacity_bytes_ &&
            protected_range_at(absolute, protected_ranges_) == nullptr) {
            read = read_exact_from_device(*target_, absolute, slice, cancellation);
        } else {
            read = read_exact_from_reader(*source_, absolute, slice, cancellation);
        }
        if (!read) {
            return read;
        }
        done += read.value();
    }
    return base::Result<std::size_t>::success(done);
}

base::Result<void>
CompositeNtfsBlockDevice::write_overlay_page(const std::uint64_t page_index,
                                             const std::uint64_t write_offset,
                                             const std::span<const std::byte> source,
                                             const base::CancellationToken cancellation) {
    const auto page_begin = page_index * detail::kOverlayPageSizeBytes;
    const auto page_size = static_cast<std::size_t>((std::min)(
        detail::kOverlayPageSizeBytes, source_logical_size_bytes_ - page_begin));
    if (overlay_index_->contains_page(page_index)) {
        return overlay_->write_at(write_offset, source, cancellation);
    }

    std::vector<std::byte> page(page_size);
    auto hydrated = read_base_at(page_begin, page, cancellation);
    if (!hydrated) {
        return base::Result<void>::failure(hydrated.error());
    }
    const auto page_offset = static_cast<std::size_t>(write_offset - page_begin);
    std::copy(source.begin(), source.end(), page.begin() + page_offset);
    auto written = overlay_->write_at(page_begin, page, cancellation);
    if (!written) {
        return written;
    }
    overlay_index_->mark_written(page_begin, page_size);
    return base::Result<void>::success();
}

base::Result<void>
CompositeNtfsBlockDevice::write_at(const std::uint64_t offset,
                                   const std::span<const std::byte> source,
                                   const base::CancellationToken cancellation) {
    if (auto range = require_range(offset, source.size(), source_logical_size_bytes_); !range) {
        return range;
    }
    if (source.empty()) {
        return base::Result<void>::success();
    }

    std::size_t position = 0;
    while (position < source.size()) {
        const auto absolute = offset + position;
        if (absolute < target_capacity_bytes_) {
            const auto target_chunk = static_cast<std::size_t>(
                (std::min)(static_cast<std::uint64_t>(source.size() - position),
                           target_capacity_bytes_ - absolute));
            if (intersects_protected(absolute, target_chunk, protected_ranges_)) {
                return base::Result<void>::failure(
                    {base::ErrorCode::kInvalidArgument, "ntfs_resize.protected_write"});
            }
            auto written = detail::write_block_device_bytes(
                *target_, absolute, source.subspan(position, target_chunk), cancellation);
            if (!written) {
                return written;
            }
            position += target_chunk;
            continue;
        }
        const auto page = detail::overlay_page_index(absolute);
        const auto page_end = (page + 1U) * detail::kOverlayPageSizeBytes;
        const auto overlay_chunk = static_cast<std::size_t>((std::min)(
            static_cast<std::uint64_t>(source.size() - position), page_end - absolute));
        auto written = write_overlay_page(page, absolute, source.subspan(position, overlay_chunk),
                                          cancellation);
        if (!written) {
            return written;
        }
        position += overlay_chunk;
    }
    return base::Result<void>::success();
}

base::Result<void>
CompositeNtfsBlockDevice::flush(const base::CancellationToken cancellation) {
    auto overlay_flush = overlay_->flush(cancellation);
    if (!overlay_flush) {
        return overlay_flush;
    }
    return target_->flush(cancellation);
}

} // namespace aegra::ntfs_resize
