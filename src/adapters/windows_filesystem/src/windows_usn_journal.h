#pragma once

#include "windows_file_handle.h"

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::adapters::windows_filesystem::detail {

/// Snapshot-bound USN journal helpers (FI2).
/// All IO uses the provided snapshot volume root; never substitutes the live volume.

/// Opens a volume handle suitable for FSCTL_QUERY/READ_USN_JOURNAL on the snapshot root.
/// root_utf16 is the snapshot device/root path (with or without trailing '\\').
[[nodiscard]] base::Result<UniqueHandle>
open_snapshot_volume_for_journal(const std::vector<std::uint16_t>& root_utf16);

/// Queries journal state on an already-open snapshot volume handle.
/// Unsupported / inactive / inaccessible journal returns available=false (not a hard error).
[[nodiscard]] base::Result<contracts::FileJournalState>
query_usn_journal_state(const UniqueHandle& volume, const std::string& volume_identity,
                        base::CancellationToken cancellation);

/// Reads a bounded change batch for half-open [start_usn, end_usn).
/// Requires an active journal; short batches set next_start_usn when more remains.
[[nodiscard]] base::Result<contracts::FileChangeBatch>
read_usn_change_batch(const UniqueHandle& volume, const std::string& volume_identity,
                      std::uint64_t journal_id, std::int64_t start_usn, std::int64_t end_usn,
                      std::uint32_t maximum_hints, base::CancellationToken cancellation);

/// Maps a Windows USN Reason mask to a platform-independent FileChangeReason.
/// Unknown or unsupported-object bits map to kAmbiguous (conservative).
[[nodiscard]] contracts::FileChangeReason map_usn_reason(std::uint32_t reason_mask) noexcept;

} // namespace aegra::adapters::windows_filesystem::detail
