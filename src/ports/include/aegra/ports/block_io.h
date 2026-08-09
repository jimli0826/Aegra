#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::ports {

enum class BlockExtentState : std::uint8_t {
    kData = 1,
    kFree = 2,
};

struct BlockExtent final {
    std::uint64_t logical_offset{0};
    std::uint64_t logical_size{0};
    BlockExtentState state{BlockExtentState::kData};
};

class IBlockSource {
  public:
    IBlockSource() = default;
    virtual ~IBlockSource() = default;
    IBlockSource(const IBlockSource&) = delete;
    IBlockSource& operator=(const IBlockSource&) = delete;
    IBlockSource(IBlockSource&&) = delete;
    IBlockSource& operator=(IBlockSource&&) = delete;

    [[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;
    /// Returns the next uniform DATA/FREE extent beginning at logical_offset, capped by
    /// maximum_size. The default implementation reports allocated DATA. FREE extents must not
    /// be passed to read() by backup pipelines.
    [[nodiscard]] virtual base::Result<BlockExtent>
    describe_extent(std::uint64_t logical_offset, std::uint64_t maximum_size,
                    base::CancellationToken cancellation) const {
        if (cancellation.stop_requested()) {
            return base::Result<BlockExtent>::failure(
                {base::ErrorCode::kCancelled, "block extent query cancelled"});
        }
        const auto size = size_bytes();
        if (maximum_size == 0 || logical_offset >= size) {
            return base::Result<BlockExtent>::failure(
                {base::ErrorCode::kInvalidArgument, "block extent query is out of range"});
        }
        return base::Result<BlockExtent>::success({logical_offset,
                                                   (std::min)(maximum_size, size - logical_offset),
                                                   BlockExtentState::kData});
    }
    [[nodiscard]] virtual base::Result<std::size_t> read(std::uint64_t offset,
                                                         std::span<std::byte> destination,
                                                         base::CancellationToken cancellation) = 0;
};

class IBlockSink {
public:
    IBlockSink() = default;
    virtual ~IBlockSink() = default;
    IBlockSink(const IBlockSink&) = delete;
    IBlockSink& operator=(const IBlockSink&) = delete;
    IBlockSink(IBlockSink&&) = delete;
    IBlockSink& operator=(IBlockSink&&) = delete;

    [[nodiscard]] virtual std::uint64_t capacity_bytes() const noexcept = 0;
    [[nodiscard]] virtual base::Result<void> write(
        std::uint64_t offset,
        std::span<const std::byte> source,
        base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<void> flush(base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
