#pragma once

#include "aegra/base/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::adapters::windows_pe {

/// Volume-stable identity of one archive file on the online system, used to build
/// PePendingJobV1 chain layers (drive letters are not stable inside WinPE) and to
/// re-check that no archive lives on a disk that the restore will overwrite.
struct PeArchiveLocation final {
    /// Canonical `\\?\Volume{...}\` path of the hosting volume.
    std::string volume_guid_utf8;
    /// Path relative to the volume root (backslashes, no leading separator).
    std::string relative_path_utf8;
    /// Physical disks backing the hosting volume (spanned volumes list several).
    std::vector<std::uint32_t> disk_numbers;
};

[[nodiscard]] base::Result<PeArchiveLocation>
locate_pe_archive_layer(const std::string& absolute_path_utf8);

} // namespace aegra::adapters::windows_pe
