#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace aegra::apps::service {

enum class WindowsServiceState : std::uint8_t {
    kStopped = 1,
    kStartPending = 2,
    kRunning = 3,
    kStopPending = 4,
};

struct WindowsServiceStatus final {
    WindowsServiceState state{WindowsServiceState::kStopped};
    std::uint32_t win32_exit_code{0};
    std::uint32_t service_exit_code{0};
    std::uint32_t wait_hint_ms{0};
    bool accepts_stop{false};
};

// Status reporter seam for ServiceMain tests. Production will wrap SetServiceStatus.
class IWindowsServiceStatusReporter {
  public:
    IWindowsServiceStatusReporter() = default;
    virtual ~IWindowsServiceStatusReporter() = default;
    IWindowsServiceStatusReporter(const IWindowsServiceStatusReporter&) = delete;
    IWindowsServiceStatusReporter& operator=(const IWindowsServiceStatusReporter&) = delete;
    IWindowsServiceStatusReporter(IWindowsServiceStatusReporter&&) = delete;
    IWindowsServiceStatusReporter& operator=(IWindowsServiceStatusReporter&&) = delete;

    [[nodiscard]] virtual base::Result<void> report(const WindowsServiceStatus& status) = 0;
};

// Owns ServiceMain/ControlHandler state without linking the process to StartServiceCtrlDispatcher.
// Composition roots call run() from a worker thread and route SERVICE_CONTROL_STOP into request_stop().
class WindowsServiceScmHost final {
  public:
    using Worker = std::function<base::Result<void>(const base::CancellationToken& cancellation)>;

    explicit WindowsServiceScmHost(std::chrono::milliseconds stop_deadline =
                                       std::chrono::milliseconds{5'000});

    [[nodiscard]] base::Result<void> run(IWindowsServiceStatusReporter& reporter, Worker worker);
    void request_stop() noexcept;
    [[nodiscard]] WindowsServiceStatus status() const;
    [[nodiscard]] base::CancellationToken cancellation_token() const noexcept;

  private:
    [[nodiscard]] base::Result<void> report_state(IWindowsServiceStatusReporter& reporter,
                                                  WindowsServiceState state, bool accepts_stop,
                                                  std::uint32_t wait_hint_ms,
                                                  std::uint32_t win32_exit_code = 0,
                                                  std::uint32_t service_exit_code = 0);

    std::chrono::milliseconds stop_deadline_;
    base::CancellationSource cancellation_;
    mutable std::mutex mutex_;
    WindowsServiceStatus status_{};
};

class RecordingServiceStatusReporter final : public IWindowsServiceStatusReporter {
  public:
    [[nodiscard]] base::Result<void> report(const WindowsServiceStatus& status) override;
    [[nodiscard]] const std::vector<WindowsServiceStatus>& history() const noexcept {
        return history_;
    }

  private:
    std::vector<WindowsServiceStatus> history_;
};

} // namespace aegra::apps::service
