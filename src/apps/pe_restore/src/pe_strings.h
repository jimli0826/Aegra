#pragma once

#include <cstdint>
#include <string_view>

namespace aegra::apps::pe_restore {

enum class StringId : std::uint8_t {
    kWindowTitle,
    kStagePreparing,
    kStageLocatingJob,
    kStageVerifying,
    kStageRestoring,
    kStageFinished,
    kSummaryHeading,
    kSummaryBackup,
    kSummaryTargetDisk,
    kSummaryWarning,
    kCountdownFormat,
    kStartNow,
    kCancelAndReboot,
    kPasswordPrompt,
    kPasswordConfirm,
    kResultSuccess,
    kResultFailed,
    kResultCancelled,
    kRebootCountdownFormat,
    kProgressFormat,
    kCancelDisabledNote,
};

/// Locale forms match Desktop translations: "en-US", "zh-CN", "zh-TW", "ja-JP", "de-DE".
/// Unknown locales fall back to English.
[[nodiscard]] const wchar_t* pe_string(StringId id, std::string_view locale);

} // namespace aegra::apps::pe_restore
