#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/process_launcher.h"

#include <cstdint>
#include <string>

namespace aegra::ntfs_resize {

enum class ChkdskMappedResult : std::uint8_t {
    kClean = 1,
    kFixed = 2,
    kCleanupWarning = 3,
    kFailed = 4,
    kOutcomeUnknown = 5,
};

struct ChkdskRunRequest final {
    ports::IProcessLauncher* launcher{nullptr};
    /// Absolute path from GetSystemDirectoryW\\chkdsk.exe (resolved by Worker).
    std::string chkdsk_executable_path;
    /// Canonical volume name, e.g. \\\\?\\Volume{guid}.
    std::string volume_name;
};

struct ChkdskRunResult final {
    ChkdskMappedResult mapped{ChkdskMappedResult::kFailed};
    std::uint32_t exit_code{0};
    bool process_terminated{false};
    bool cancel_observed_but_ignored{false};
    std::string message_code{"restore.shrink_postcheck_failed"};
    /// Condensed CHKDSK stdout/stderr (single line, truncated); diagnostic only.
    std::string output_excerpt;
};

/// Runs `chkdsk /x /f <volume>`. Ordinary cancel must not terminate the process; wait continues
/// until exit, then maps the exit code per ADR-0025.
[[nodiscard]] base::Result<ChkdskRunResult>
run_shrink_chkdsk(const ChkdskRunRequest& request, base::CancellationToken cancellation);

} // namespace aegra::ntfs_resize
