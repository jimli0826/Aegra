#pragma once

#include "aegra/adapters/windows_filesystem/windows_filesystem.h"
#include "aegra/contracts/file_set.h"

#include <string>
#include <vector>

namespace aegra::adapters::windows_filesystem::detail {

/// Live-browse node for a user profile special folder (Desktop, Documents, …).
struct SpecialFolderBrowseRoot final {
    std::string volume_identity;
    std::vector<contracts::EncodedName> relative_components;
    std::wstring absolute_path;
    std::string display_name;
};

/// Resolves the six Explorer quick-access folders against authorized volume roots.
/// Folders that cannot be mapped to a root (missing path, other volume) are omitted.
[[nodiscard]] std::vector<SpecialFolderBrowseRoot>
resolve_special_folder_browse_roots(const std::vector<SnapshotVolumeBinding>& volume_roots);

} // namespace aegra::adapters::windows_filesystem::detail
