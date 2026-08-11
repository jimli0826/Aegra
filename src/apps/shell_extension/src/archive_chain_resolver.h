#pragma once

#include "aegra/base/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aegra::shell {

/// Product limit S11: standalone repository-parent walk.
inline constexpr std::uint32_t kMaximumRepositoryAncestorWalk = 16;

struct ResolvedArchiveChain final {
    /// Base-first absolute paths of every layer (Full root … tip).
    std::vector<std::filesystem::path> layer_paths;
    std::string tip_file_uuid;
};

/// Locate managed Local Repository for an Incremental tip and resolve base-first layer paths.
/// Fails with kNotFound / message shell.parent_missing when repository or chain is incomplete.
/// Does not open Archive readers and never accepts passwords.
[[nodiscard]] base::Result<ResolvedArchiveChain>
resolve_managed_archive_chain(const std::filesystem::path& tip_archive_path,
                              const std::array<std::byte, 16>& tip_file_uuid);

} // namespace aegra::shell
