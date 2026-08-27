#include "aegra/apps/service/service_security_host.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

std::atomic<std::uint64_t> g_next_session_serial{1};

[[nodiscard]] ServiceSessionContext make_session_context() {
    ServiceSessionContext session;
    session.browser_session.session_id =
        "pipe-session-" +
        std::to_string(g_next_session_serial.fetch_add(1, std::memory_order_relaxed));
    return session;
}

void log_session(const ServiceRuntimeInfo& runtime, const ServiceLogLevel level,
                 const std::string_view message_code, const std::string_view detail) noexcept {
    if (runtime.logger != nullptr) {
        runtime.logger->write(level, message_code, detail);
    }
}

// One accepted client served on its own thread. The named pipe listener creates a fresh
// PIPE_UNLIMITED_INSTANCES instance per accept, so serving each connection concurrently lets
// Desktop hold a long-lived session while the CLI (and other clients) connect at the same time.
// `finished()` lets the host reap completed workers without blocking; the shared flag flips true
// only after run_service_session returns, so a reaped worker joins immediately.
class SessionWorker final {
  public:
    SessionWorker(std::unique_ptr<adapters::windows_ipc::WindowsNamedPipeChannel> channel,
                  const ServiceRuntimeInfo& runtime, const std::size_t maximum_requests_per_session,
                  const base::CancellationToken& cancellation)
        : done_(std::make_shared<std::atomic_bool>(false)) {
        auto done = done_;
        thread_ = std::jthread([channel = std::move(channel), &runtime,
                                maximum_requests_per_session, &cancellation, done]() mutable {
            const auto session = make_session_context();
            auto result = run_service_session(*channel, runtime, session, cancellation,
                                              maximum_requests_per_session);
            if (!result && result.error().code != base::ErrorCode::kCancelled &&
                result.error().code != base::ErrorCode::kIoFailure) {
                log_session(runtime, ServiceLogLevel::kWarning, "service.session_failed",
                            "session_id=" + session.browser_session.session_id);
            }
            done->store(true, std::memory_order_release);
        });
    }

    [[nodiscard]] bool finished() const noexcept {
        return done_->load(std::memory_order_acquire);
    }

  private:
    std::shared_ptr<std::atomic_bool> done_;
    std::jthread thread_;
};

} // namespace

base::Result<void> run_service_host(adapters::windows_ipc::WindowsNamedPipeListener& listener,
                                    const ServiceRuntimeInfo& runtime,
                                    const ServiceHostOptions& options,
                                    const base::CancellationToken& cancellation) {
    if (options.stop_deadline.count() <= 0) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "service stop deadline is invalid"});
    }

    // Once mode stays strictly serial: accept and serve a single session inline so controlled
    // diagnostics observe exactly one connection.
    if (options.once) {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                base::Error{base::ErrorCode::kCancelled, "service host stop requested"});
        }
        auto accepted = listener.accept(cancellation);
        if (!accepted) {
            return base::Result<void>::failure(accepted.error());
        }
        const auto session = make_session_context();
        return run_service_session(*accepted.value(), runtime, session, cancellation,
                                   options.maximum_requests_per_session);
    }

    // Forever mode: each accepted client is served on its own thread so a long-lived Desktop
    // session never blocks a concurrent CLI connection. The accept loop reaps finished workers
    // and only stops on cancellation or a fatal accept failure; per-session transport failures
    // stay confined to their own thread and never tear down the host.
    std::vector<SessionWorker> sessions;
    for (;;) {
        if (cancellation.stop_requested()) {
            break;
        }
        auto accepted = listener.accept(cancellation);
        if (!accepted) {
            if (accepted.error().code == base::ErrorCode::kCancelled) {
                break;
            }
            log_session(runtime, ServiceLogLevel::kError, "service.accept_failed", "mode=service");
            return base::Result<void>::failure(accepted.error());
        }
        std::erase_if(sessions, [](const SessionWorker& worker) { return worker.finished(); });
        sessions.emplace_back(std::move(accepted.value()), runtime,
                              options.maximum_requests_per_session, cancellation);
    }

    // Cancellation wakes every session's pending receive; wait for them to unwind within the stop
    // deadline. Remaining workers are joined by the vector destructor regardless, so this bounds
    // how long we report as clean shutdown rather than abandoning threads.
    const auto end = std::chrono::steady_clock::now() + options.stop_deadline;
    while (std::chrono::steady_clock::now() < end) {
        std::erase_if(sessions, [](const SessionWorker& worker) { return worker.finished(); });
        if (sessions.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!sessions.empty()) {
        log_session(runtime, ServiceLogLevel::kWarning, "service.stop_deadline_exceeded",
                    "pending_sessions=" + std::to_string(sessions.size()));
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInternal, "service host stop deadline exceeded"});
    }
    return base::Result<void>::failure(
        base::Error{base::ErrorCode::kCancelled, "service host stop requested"});
}

} // namespace aegra::apps::service
