#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_core/types.h"
#include "aegra/ntfs_resize/composite_ntfs_block_device.h"
#include "aegra/ports/random_access.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace aegra::ntfs_resize::detail {

/// Loads/stores one MFT record through the composite view using $MFT data runs.
class MftRecordStore final {
  public:
    MftRecordStore(CompositeNtfsBlockDevice& device, ntfs_core::BootGeometry geometry,
                   ntfs_core::AttributeValue mft_data);
    MftRecordStore(ports::IRandomAccessReader& reader, ntfs_core::BootGeometry geometry,
                   ntfs_core::AttributeValue mft_data);

    [[nodiscard]] base::Result<std::vector<std::byte>>
    read_record_bytes(std::uint64_t record_number, base::CancellationToken cancellation);

    [[nodiscard]] base::Result<void>
    write_record_bytes(std::uint64_t record_number, std::span<std::byte> record_bytes,
                       base::CancellationToken cancellation);

    [[nodiscard]] const ntfs_core::BootGeometry& geometry() const noexcept {
        return geometry_;
    }
    void set_mft_data(ntfs_core::AttributeValue mft_data) {
        mft_data_ = std::move(mft_data);
    }

  private:
    CompositeNtfsBlockDevice* device_;
    ports::IRandomAccessReader* reader_{nullptr};
    ntfs_core::BootGeometry geometry_{};
    ntfs_core::AttributeValue mft_data_{};
};

} // namespace aegra::ntfs_resize::detail
