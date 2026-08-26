#pragma once

#include "aegra/base/result.h"

#include <cstdint>
#include <string>

namespace aegra::apps::pe_restore {

enum class PeRunStage : std::uint8_t {
    kLocatingJob = 1,
    kVerifying = 2,
    kRestoring = 3,
};

struct PeRunSummary final {
    std::string locale;
    std::string backup_display;
    std::string target_display;
    std::uint32_t auto_start_seconds{10};
    bool needs_password{false};
};

enum class PeUserDecision : std::uint8_t {
    kStart = 1,
    kCancel = 2,
};

struct PeRunProgress final {
    bool has_percent{false};
    std::uint32_t percent{0};
    std::uint64_t written_bytes{0};
};

enum class PeRunResultKind : std::uint8_t {
    kSuccess = 1,
    kFailed = 2,
    kCancelled = 3,
};

struct PeRunOutcome final {
    PeRunResultKind kind{PeRunResultKind::kFailed};
    /// Stable `pe_restore.*` code (empty on success).
    std::string error_code;
    std::string error_message;
    std::string locale;
    /// Success and cancel reboot automatically; failure stays on the error page.
    bool reboot{false};
};

/// UI bridge. All methods are invoked on the orchestration thread; blocking
/// methods must pump their signal from the UI thread (event + message posting).
class IPeRunView {
  public:
    IPeRunView() = default;
    virtual ~IPeRunView() = default;
    IPeRunView(const IPeRunView&) = delete;
    IPeRunView& operator=(const IPeRunView&) = delete;
    IPeRunView(IPeRunView&&) = delete;
    IPeRunView& operator=(IPeRunView&&) = delete;

    virtual void stage_changed(PeRunStage stage) = 0;
    /// Shows the summary + countdown page and blocks until the user decides or
    /// the countdown elapses (elapse = kStart).
    [[nodiscard]] virtual PeUserDecision wait_start_decision(const PeRunSummary& summary) = 0;
    /// Prompt-mode password entry; kCancelled failure when the user cancels.
    [[nodiscard]] virtual base::Result<std::string> wait_password() = 0;
    virtual void progress_changed(const PeRunProgress& progress) = 0;
};

/// Runs the complete PE restore flow on the calling thread and returns the
/// terminal outcome. The restore result document is persisted before returning
/// whenever a pending store was located.
[[nodiscard]] PeRunOutcome run_pe_restore_flow(IPeRunView& view);

/// Reboots the machine from WinPE (wpeutil; best-effort).
void reboot_winpe();

} // namespace aegra::apps::pe_restore
