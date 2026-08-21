#include "block_device_byte_io.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace aegra::ntfs_resize::detail {
namespace {

struct AlignedRange final {
    std::uint64_t offset{0};
    std::size_t size{0};
    std::size_t payload_offset{0};
};

[[nodiscard]] base::Result<AlignedRange>
align_byte_range(const ports::BlockDeviceGeometry& geometry, const std::uint64_t offset,
                 const std::size_t size) {
    const auto alignment = static_cast<std::uint64_t>(geometry.logical_sector_size);
    if (alignment == 0 || offset > geometry.capacity_bytes ||
        size > geometry.capacity_bytes - offset) {
        return base::Result<AlignedRange>::failure(
            {base::ErrorCode::kInvalidArgument, "ntfs_resize.block_range_invalid"});
    }
    if (size == 0) {
        return base::Result<AlignedRange>::success(AlignedRange{offset, 0, 0});
    }
    const auto request_end = offset + size;
    const auto aligned_offset = offset - (offset % alignment);
    const auto end_remainder = request_end % alignment;
    const auto end_padding = end_remainder == 0 ? 0 : alignment - end_remainder;
    if (end_padding > geometry.capacity_bytes - request_end) {
        return base::Result<AlignedRange>::failure(
            {base::ErrorCode::kInvalidArgument, "ntfs_resize.block_range_invalid"});
    }
    const auto aligned_end = request_end + end_padding;
    const auto aligned_size = aligned_end - aligned_offset;
    if (aligned_size > (std::numeric_limits<std::size_t>::max)()) {
        return base::Result<AlignedRange>::failure(
            {base::ErrorCode::kInvalidArgument, "ntfs_resize.block_range_invalid"});
    }
    return base::Result<AlignedRange>::success(AlignedRange{
        aligned_offset, static_cast<std::size_t>(aligned_size),
        static_cast<std::size_t>(offset - aligned_offset)});
}

[[nodiscard]] bool is_direct_range(const AlignedRange& aligned,
                                   const std::size_t payload_size) noexcept {
    return aligned.payload_offset == 0 && aligned.size == payload_size;
}

} // namespace

base::Result<std::size_t>
read_block_device_bytes(ports::IRandomAccessBlockDevice& device, const std::uint64_t offset,
                        const std::span<std::byte> destination,
                        const base::CancellationToken cancellation) {
    auto aligned = align_byte_range(device.geometry(), offset, destination.size());
    if (!aligned) {
        return base::Result<std::size_t>::failure(aligned.error());
    }
    if (destination.empty()) {
        return base::Result<std::size_t>::success(0);
    }
    if (is_direct_range(aligned.value(), destination.size())) {
        return device.read_at(offset, destination, cancellation);
    }
    std::vector<std::byte> bounce(aligned.value().size);
    auto read = device.read_at(aligned.value().offset, bounce, cancellation);
    if (!read) {
        return base::Result<std::size_t>::failure(read.error());
    }
    if (read.value() != bounce.size()) {
        return base::Result<std::size_t>::failure(
            {base::ErrorCode::kIoFailure, "ntfs_resize.block_short_read"});
    }
    const auto payload_begin = bounce.begin() +
                               static_cast<std::ptrdiff_t>(aligned.value().payload_offset);
    std::copy_n(payload_begin, destination.size(), destination.begin());
    return base::Result<std::size_t>::success(destination.size());
}

base::Result<void>
write_block_device_bytes(ports::IRandomAccessBlockDevice& device, const std::uint64_t offset,
                         const std::span<const std::byte> source,
                         const base::CancellationToken cancellation) {
    auto aligned = align_byte_range(device.geometry(), offset, source.size());
    if (!aligned) {
        return base::Result<void>::failure(aligned.error());
    }
    if (source.empty()) {
        return base::Result<void>::success();
    }
    if (is_direct_range(aligned.value(), source.size())) {
        return device.write_at(offset, source, cancellation);
    }
    std::vector<std::byte> bounce(aligned.value().size);
    auto read = device.read_at(aligned.value().offset, bounce, cancellation);
    if (!read) {
        return base::Result<void>::failure(read.error());
    }
    if (read.value() != bounce.size()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "ntfs_resize.block_short_read"});
    }
    auto payload_begin = bounce.begin() +
                         static_cast<std::ptrdiff_t>(aligned.value().payload_offset);
    std::copy(source.begin(), source.end(), payload_begin);
    return device.write_at(aligned.value().offset, bounce, cancellation);
}

} // namespace aegra::ntfs_resize::detail
