#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/shrink_plan.h"
#include "aegra/ports/random_access.h"
#include "aegra/ports/random_access_block_device.h"
#include "aegra/ports/scratch_store.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace aegra::ntfs_resize {

namespace detail {
class SparseOverlayIndex;
}

/// Construction inputs for a composite source-logical volume view (ADR-0025).
struct CompositeNtfsBlockDeviceConfig final {
    ports::IRandomAccessBlockDevice* target{nullptr};
    ports::IRandomAccessReader* source{nullptr};
    ports::IScratchStore* overlay{nullptr};
    std::uint64_t source_logical_size_bytes{0};
    /// Copied on create; writes intersecting these ranges on the target path fail closed.
    std::span<const ByteRange> protected_ranges{};
};

/// Random-access view of the source logical volume after prefix restore during shrink.
///
/// Capacity fields stay separated: `source_logical_size_bytes()` is the archive merged size;
/// `target_capacity_bytes()` is always the true target device capacity (never faked).
///
/// Read precedence per page: overlay (if marked written) → target prefix (except protected
/// Boot ranges, which always come from archive/source escrow) → archive source tail.
/// Writes at offset >= target capacity go only to overlay; writes below go to target (split at
/// the boundary). Target writes intersecting protected ranges return `ntfs_resize.protected_write`.
class CompositeNtfsBlockDevice final {
  public:
    CompositeNtfsBlockDevice(const CompositeNtfsBlockDevice&) = delete;
    CompositeNtfsBlockDevice& operator=(const CompositeNtfsBlockDevice&) = delete;
    CompositeNtfsBlockDevice(CompositeNtfsBlockDevice&&) noexcept;
    CompositeNtfsBlockDevice& operator=(CompositeNtfsBlockDevice&&) = delete;
    ~CompositeNtfsBlockDevice();

    [[nodiscard]] static base::Result<CompositeNtfsBlockDevice>
    create(const CompositeNtfsBlockDeviceConfig& config);

    [[nodiscard]] std::uint64_t source_logical_size_bytes() const noexcept {
        return source_logical_size_bytes_;
    }

    [[nodiscard]] std::uint64_t target_capacity_bytes() const noexcept {
        return target_capacity_bytes_;
    }

    [[nodiscard]] base::Result<std::size_t>
    read_at(std::uint64_t offset, std::span<std::byte> destination,
            base::CancellationToken cancellation);

    [[nodiscard]] base::Result<void> write_at(std::uint64_t offset, std::span<const std::byte> source,
                                              base::CancellationToken cancellation);

    [[nodiscard]] base::Result<void> flush(base::CancellationToken cancellation);

  private:
    CompositeNtfsBlockDevice(ports::IRandomAccessBlockDevice& target,
                             ports::IRandomAccessReader& source, ports::IScratchStore& overlay,
                             std::uint64_t source_logical_size_bytes,
                             std::uint64_t target_capacity_bytes,
                             std::vector<ByteRange> protected_ranges);

    [[nodiscard]] base::Result<std::size_t>
    read_base_at(std::uint64_t offset, std::span<std::byte> destination,
                 base::CancellationToken cancellation);
    [[nodiscard]] base::Result<void>
    write_overlay_page(std::uint64_t page_index, std::uint64_t write_offset,
                       std::span<const std::byte> source,
                       base::CancellationToken cancellation);

    ports::IRandomAccessBlockDevice* target_{nullptr};
    ports::IRandomAccessReader* source_{nullptr};
    ports::IScratchStore* overlay_{nullptr};
    std::uint64_t source_logical_size_bytes_{0};
    std::uint64_t target_capacity_bytes_{0};
    std::vector<ByteRange> protected_ranges_{};
    std::unique_ptr<detail::SparseOverlayIndex> overlay_index_{};
};

} // namespace aegra::ntfs_resize
