#pragma once

#include "windows_personal_backup_runtime.h"

#include "aegra/base/result.h"
#include "aegra/format/manifest.h"

#include <string_view>
#include <vector>

namespace aegra::apps::worker::detail {

/// Best-effort host probe. Leaves boot_profile absent when the selected sources are not a complete,
/// supported Windows system disk; infrastructure/hash failures are returned.
[[nodiscard]] base::Result<void>
collect_windows_boot_profile(std::string_view application_version,
                             const std::vector<PreparedVolumeMetadata>& sources,
                             format::Manifest& manifest);

} // namespace aegra::apps::worker::detail
