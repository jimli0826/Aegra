#pragma once

#include "ntfs_record_writer.h"

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/composite_ntfs_block_device.h"

namespace aegra::ntfs_resize::detail {

/// Copies the first four FILE records from $MFT into $MFTMirr data runs.
[[nodiscard]] base::Result<void>
sync_mft_mirror(CompositeNtfsBlockDevice& device, MftRecordStore& mft_store,
                base::CancellationToken cancellation);

} // namespace aegra::ntfs_resize::detail
