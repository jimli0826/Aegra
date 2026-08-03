#include "aegra/apps/service/windows_service_scm_host.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace aegra::apps::service {
namespace {

struct WorkerCompletion final {
    std::mutex mutex;
    std::condition_variable changed;
    bool finished{false};
    base::Result<void> result = base::Result<void>::success();
};

[[nodiscard]] base::Result<void>
run_worker(WindowsServiceScmHost::Worker& worker,
           const base::CancellationToken& cancellation) noexcept {
    try {
        return worker(cancellation);
    } catch (const std::exception&) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInternal, "windows service worker failed"});
    } catch (...) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInternal, "windows service worker failed"});
    }
}

} // namespace

WindowsServiceScmHost::WindowsServiceScmHost(const std::chrono::milliseconds stop_deadline)
    : stop_deadline_(stop_deadline) {}

base::Result<void> WindowsServiceScmHost::report_state(IWindowsServiceStatusReporter& reporter,
                                                       const WindowsServiceState state,
                                                       const bool accepts_stop,
                                                       const std::uint32_t wait_hint_ms,
                                                       const std::uint32_t win32_exit_code,
                                                       const std::uint32_t service_exit_code) {
    WindowsServiceStatus next;
    next.state = state;
    next.accepts_stop = accepts_stop;
    next.wait_hint_ms = wait_hint_ms;
    next.win32_exit_code = win32_exit_code;
    next.service_exit_code = service_exit_code;
    {
        std::lock_guard lock(mutex_);
        status_ = next;
    }
    return reporter.report(next);
}

base::Result<void> WindowsServiceScmHost::run(IWindowsServiceStatusReporter& reporter,
                                              Worker worker) {
    if (stop_deadline_.count() <= 0 || !worker) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument, "windows service host options are invalid"});
    }
    cancellation_ = base::CancellationSource{};
    auto pending = report_state(reporter, WindowsServiceState::kStartPending, false,
                                static_cast<std::uint32_t>(stop_deadline_.count()));
    if (!pending) {
        return pending;
    }
    auto running = report_state(reporter, WindowsServiceState::kRunning, true, 0);
    if (!running) {
        return running;
    }

    auto completion = std::make_shared<WorkerCompletion>();
    std::thread worker_thread([completion, worker = std::move(worker),
                               token = cancellation_.get_token()]() mutable {
        auto result = run_worker(worker, token);
        {
            std::lock_guard lock(completion->mutex);
            completion->result = std::move(result);
            completion->finished = true;
        }
        completion->changed.notify_all();
    });

    {
        std::unique_lock lock(completion->mutex);
        while (!completion->finished && !cancellation_.stop_requested()) {
            completion->changed.wait_for(lock, std::chrono::milliseconds(50));
        }
    }

    auto stop_pending = report_state(reporter, WindowsServiceState::kStopPending, false,
                                     static_cast<std::uint32_t>(stop_deadline_.count()));

    bool finished = false;
    {
        std::unique_lock lock(completion->mutex);
        if (completion->finished) {
            finished = true;
        } else {
            finished = completion->changed.wait_for(lock, stop_deadline_,
                                                    [&] { return completion->finished; });
        }
    }

    if (!finished) {
        auto stopped =
            report_state(reporter, WindowsServiceState::kStopped, false, 0,
                         static_cast<std::uint32_t>(base::ErrorCode::kInternal), 1);
        if (worker_thread.joinable()) {
            // Non-cooperative workers are abandoned so ServiceMain can exit within the deadline.
            worker_thread.detach();
        }
        if (!stopped) {
            return stopped;
        }
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInternal, "windows service stop deadline exceeded"});
    }

    if (worker_thread.joinable()) {
        worker_thread.join();
    }

    base::Result<void> work = base::Result<void>::success();
    {
        std::lock_guard lock(completion->mutex);
        work = std::move(completion->result);
    }
    if (!work) {
        const auto cancelled = work.error().code == base::ErrorCode::kCancelled;
        const auto code = cancelled ? 0U : static_cast<std::uint32_t>(work.error().code);
        auto stopped =
            report_state(reporter, WindowsServiceState::kStopped, false, 0, code, cancelled ? 0U : 1U);
        if (!stopped) {
            return stopped;
        }
        if (!stop_pending) {
            return stop_pending;
        }
        return cancelled ? base::Result<void>::success()
                         : base::Result<void>::failure(work.error());
    }
    auto stopped = report_state(reporter, WindowsServiceState::kStopped, false, 0);
    if (!stopped) {
        return stopped;
    }
    if (!stop_pending) {
        return stop_pending;
    }
    return base::Result<void>::success();
}

void WindowsServiceScmHost::request_stop() noexcept { cancellation_.request_stop(); }

WindowsServiceStatus WindowsServiceScmHost::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

base::CancellationToken WindowsServiceScmHost::cancellation_token() const noexcept {
    return cancellation_.get_token();
}

base::Result<void> RecordingServiceStatusReporter::report(const WindowsServiceStatus& status) {
    history_.push_back(status);
    return base::Result<void>::success();
}

} // namespace aegra::apps::service
