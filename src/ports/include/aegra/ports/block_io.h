#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::ports {

class IBlockSource {
public:
    IBlockSource() = default;
    virtual ~IBlockSource() = default;
    IBlockSource(const IBlockSource&) = delete;
    IBlockSource& operator=(const IBlockSource&) = delete;
    IBlockSource(IBlockSource&&) = delete;
    IBlockSource& operator=(IBlockSource&&) = delete;

    [[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;
    [[nodiscard]] virtual base::Result<std::size_t> read(
        std::uint64_t offset,
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
