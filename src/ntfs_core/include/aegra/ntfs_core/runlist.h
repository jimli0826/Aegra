#pragma once

#include "aegra/base/result.h"
#include "aegra/ntfs_core/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aegra::ntfs_core {

[[nodiscard]] base::Result<std::vector<DataRun>>
parse_runlist(std::span<const std::byte> runlist, VirtualClusterNumber first_vcn,
              VirtualClusterNumber last_vcn);

/// Validates VCN continuity, non-zero lengths, LCN deltas, and absence of VCN/LCN overlap.
[[nodiscard]] base::Result<void> validate_data_runs(std::span<const DataRun> runs);

/// Computes encoded size including the 0x00 terminator. Does not allocate a run buffer.
[[nodiscard]] base::Result<std::size_t> measure_runlist_encoded_size(std::span<const DataRun> runs);

/// Writes the encoded runlist into destination. Fails if destination is too small.
/// Returns bytes written (including terminator).
[[nodiscard]] base::Result<std::size_t> encode_runlist(std::span<const DataRun> runs,
                                                       std::span<std::byte> destination);

/// Allocates a bounded vector and encodes. Rejects when encoded size exceeds maximum_bytes.
[[nodiscard]] base::Result<std::vector<std::byte>>
encode_runlist_bounded(std::span<const DataRun> runs, std::size_t maximum_bytes);

} // namespace aegra::ntfs_core
