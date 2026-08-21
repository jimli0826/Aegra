#pragma once

#include "ntfs_record_writer.h"

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/composite_ntfs_block_device.h"

namespace aegra::ntfs_resize::detail {

/// Resets the whole $LogFile to 0xFF (the ntfsresize convention): Windows cannot replay stale
/// LSNs against rewritten metadata and reinitializes the pristine log on the next mount without
/// marking the volume dirty, so no CHKDSK pass is required (ADR-0025).
[[nodiscard]] base::Result<void>
invalidate_logfile_restart_area(CompositeNtfsBlockDevice& device, MftRecordStore& mft_store,
                                base::CancellationToken cancellation);

} // namespace aegra::ntfs_resize::detail
