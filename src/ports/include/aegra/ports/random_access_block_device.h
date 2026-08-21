#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/block_io.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::ports {

/// Physical geometry of a random-access block device.
/// capacity_bytes is always the true writable capacity — never a faked source size.
struct BlockDeviceGeometry final {
    std::uint32_t logical_sector_size{0};
    std::uint32_t physical_sector_size{0};
    std::uint64_t capacity_bytes{0};
};

/// Random-access block device with read, write, flush, and geometry.
/// Kept separate from IBlockSink so existing write-only consumers stay free of
/// platform geometry and readback requirements. Composition roots may adapt a
/// concrete device to IBlockSink via a thin view when only write is needed.
class IRandomAccessBlockDevice {
  public:
    IRandomAccessBlockDevice() = default;
    virtual ~IRandomAccessBlockDevice() = default;
    IRandomAccessBlockDevice(const IRandomAccessBlockDevice&) = delete;
    IRandomAccessBlockDevice& operator=(const IRandomAccessBlockDevice&) = delete;
    IRandomAccessBlockDevice(IRandomAccessBlockDevice&&) = delete;
    IRandomAccessBlockDevice& operator=(IRandomAccessBlockDevice&&) = delete;

    [[nodiscard]] virtual BlockDeviceGeometry geometry() const noexcept = 0;

    /// Reads destination.size() bytes at offset. Short reads are failures.
    [[nodiscard]] virtual base::Result<std::size_t>
    read_at(std::uint64_t offset, std::span<std::byte> destination,
            base::CancellationToken cancellation) = 0;

    /// Writes the full source buffer at offset. Ranges must lie within capacity.
    [[nodiscard]] virtual base::Result<void>
    write_at(std::uint64_t offset, std::span<const std::byte> source,
             base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void> flush(base::CancellationToken cancellation) = 0;
};

/// Adapts IRandomAccessBlockDevice to the existing IBlockSink write contract.
class RandomAccessBlockDeviceSink final : public IBlockSink {
  public:
    explicit RandomAccessBlockDeviceSink(IRandomAccessBlockDevice& device) noexcept
        : device_(&device) {}

    [[nodiscard]] std::uint64_t capacity_bytes() const noexcept override {
        return device_->geometry().capacity_bytes;
    }

    [[nodiscard]] base::Result<void> write(std::uint64_t offset, std::span<const std::byte> source,
                                           base::CancellationToken cancellation) override {
        return device_->write_at(offset, source, cancellation);
    }

    [[nodiscard]] base::Result<void> flush(base::CancellationToken cancellation) override {
        return device_->flush(cancellation);
    }

  private:
    IRandomAccessBlockDevice* device_;
};

} // namespace aegra::ports
