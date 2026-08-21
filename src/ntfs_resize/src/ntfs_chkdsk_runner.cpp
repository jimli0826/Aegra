#include "aegra/ntfs_resize/ntfs_chkdsk_runner.h"

#include "ntfs_shrink_errors.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::ntfs_resize {
namespace {

constexpr std::size_t kMaxOutputExcerptBytes = 1024;

// Collapse the captured multi-line console output into one bounded log-friendly line.
[[nodiscard]] std::string condense_output(const std::string_view text) {
    std::string result;
    result.reserve((std::min)(text.size(), kMaxOutputExcerptBytes));
    bool pending_separator = false;
    for (const char character : text) {
        if (result.size() >= kMaxOutputExcerptBytes) {
            result.append("...");
            break;
        }
        if (character == '\r') {
            continue;
        }
        if (character == '\n') {
            pending_separator = !result.empty();
            continue;
        }
        if (pending_separator) {
            result.append(" | ");
            pending_separator = false;
        }
        result.push_back(character);
    }
    return result;
}

[[nodiscard]] ChkdskRunResult map_exit_code(const ports::ProcessExitStatus& status) {
    ChkdskRunResult result;
    result.exit_code = status.exit_code;
    result.process_terminated = status.terminated;
    result.output_excerpt = condense_output(status.output);
    if (status.terminated) {
        result.mapped = ChkdskMappedResult::kOutcomeUnknown;
        result.message_code = "restore.shrink_commit_outcome_unknown";
        return result;
    }
    switch (status.exit_code) {
    case 0:
        result.mapped = ChkdskMappedResult::kClean;
        result.message_code = "restore.shrink_chkdsk_clean";
        break;
    case 1:
        result.mapped = ChkdskMappedResult::kFixed;
        result.message_code = "restore.shrink_chkdsk_fixed";
        break;
    case 2:
        result.mapped = ChkdskMappedResult::kCleanupWarning;
        result.message_code = "restore.shrink_chkdsk_cleanup";
        break;
    case 3:
        result.mapped = ChkdskMappedResult::kFailed;
        result.message_code = "restore.shrink_postcheck_failed";
        break;
    default:
        result.mapped = ChkdskMappedResult::kFailed;
        result.message_code = "restore.shrink_postcheck_failed";
        break;
    }
    return result;
}

[[nodiscard]] base::Result<ports::ProcessExitStatus>
wait_ignoring_cancel(ports::IProcessLauncher& launcher, const std::uint32_t pid,
                     const base::CancellationToken cancellation, bool& cancel_ignored) {
    cancel_ignored = false;
    for (;;) {
        auto waited = launcher.wait(pid, cancellation);
        if (waited) {
            return waited;
        }
        if (waited.error().code == base::ErrorCode::kCancelled) {
            cancel_ignored = true;
            // Do not terminate CHKDSK; continue waiting with a never-cancelling token.
            auto forced = launcher.wait(pid, base::CancellationToken{});
            if (forced) {
                return forced;
            }
            return forced;
        }
        return waited;
    }
}

} // namespace

base::Result<ChkdskRunResult> run_shrink_chkdsk(const ChkdskRunRequest& request,
                                               const base::CancellationToken cancellation) {
    if (request.launcher == nullptr || request.chkdsk_executable_path.empty() ||
        request.volume_name.empty()) {
        return detail::shrink_fail<ChkdskRunResult>(base::ErrorCode::kInvalidArgument,
                                                    "restore.shrink_postcheck_failed");
    }

    ports::ProcessLaunchRequest launch;
    launch.executable_path = request.chkdsk_executable_path;
    launch.arguments = {"/x", "/f", request.volume_name};
    launch.capture_output = true;
    auto started = request.launcher->launch(launch);
    if (!started) {
        return base::Result<ChkdskRunResult>::failure(started.error());
    }

    bool cancel_ignored = false;
    auto exited =
        wait_ignoring_cancel(*request.launcher, started.value().pid, cancellation, cancel_ignored);
    if (!exited) {
        ChkdskRunResult result;
        result.mapped = ChkdskMappedResult::kOutcomeUnknown;
        result.message_code = "restore.shrink_commit_outcome_unknown";
        result.cancel_observed_but_ignored = cancel_ignored;
        return base::Result<ChkdskRunResult>::success(result);
    }

    auto mapped = map_exit_code(exited.value());
    mapped.cancel_observed_but_ignored = cancel_ignored;
    return base::Result<ChkdskRunResult>::success(std::move(mapped));
}

} // namespace aegra::ntfs_resize
