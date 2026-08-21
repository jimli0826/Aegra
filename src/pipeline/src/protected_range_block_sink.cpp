#include "aegra/pipeline/protected_range_block_sink.h"

#include <algorithm>
#include <utility>

namespace aegra::pipeline {
namespace {

[[nodiscard]] bool valid_range(const ProtectedByteRange& range) noexcept {
    return range.end > range.begin;
}

[[nodiscard]] base::Result<void>
write_unprotected_span(ports::IBlockSink& sink, std::uint64_t offset,
                       std::span<const std::byte> source,
                       const std::vector<ProtectedByteRange>& protected_ranges,
                       base::CancellationToken cancellation) {
    if (source.empty()) {
        return base::Result<void>::success();
    }
    const auto write_begin = offset;
    const auto write_end = offset + source.size();
    std::uint64_t cursor = write_begin;
    for (const auto& range : protected_ranges) {
        if (!valid_range(range) || range.end <= cursor || range.begin >= write_end) {
            continue;
        }
        if (range.begin > cursor) {
            const auto size = static_cast<std::size_t>(range.begin - cursor);
            const auto payload_offset = static_cast<std::size_t>(cursor - write_begin);
            auto written =
                sink.write(cursor, source.subspan(payload_offset, size), cancellation);
            if (!written) {
                return written;
            }
        }
        cursor = (std::max)(cursor, range.end);
        if (cursor >= write_end) {
            return base::Result<void>::success();
        }
    }
    if (cursor < write_end) {
        const auto size = static_cast<std::size_t>(write_end - cursor);
        const auto payload_offset = static_cast<std::size_t>(cursor - write_begin);
        return sink.write(cursor, source.subspan(payload_offset, size), cancellation);
    }
    return base::Result<void>::success();
}

} // namespace

ProtectedRangeBlockSink::ProtectedRangeBlockSink(
    ports::IBlockSink& inner, std::vector<ProtectedByteRange> protected_ranges) noexcept
    : inner_(&inner), protected_ranges_(std::move(protected_ranges)) {
    std::sort(protected_ranges_.begin(), protected_ranges_.end(),
              [](const ProtectedByteRange& left, const ProtectedByteRange& right) {
                  return left.begin < right.begin;
              });
}

std::uint64_t ProtectedRangeBlockSink::capacity_bytes() const noexcept {
    return inner_->capacity_bytes();
}

base::Result<void> ProtectedRangeBlockSink::write(const std::uint64_t offset,
                                                  const std::span<const std::byte> source,
                                                  const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCancelled, "protected range write cancelled"});
    }
    if (offset > inner_->capacity_bytes() || source.size() > inner_->capacity_bytes() - offset) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "protected range write is out of capacity"});
    }
    return write_unprotected_span(*inner_, offset, source, protected_ranges_, cancellation);
}

base::Result<void>
ProtectedRangeBlockSink::flush(const base::CancellationToken cancellation) {
    return inner_->flush(cancellation);
}

} // namespace aegra::pipeline
