#include "aegra/apps/service/service_security_host.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

namespace aegra::apps::service {
namespace {

std::atomic<std::uint64_t> g_next_session_serial{1};

[[nodiscard]] bool wait_until_stopped(const base::CancellationToken& cancellation,
                                      const std::chrono::milliseconds deadline) {
    const auto end = std::chrono::steady_clock::now() + deadline;
    while (!cancellation.stop_requested()) {
        if (std::chrono::steady_clock::now() >= end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

[[nodiscard]] ServiceSessionContext make_session_context() {
    ServiceSessionContext session;
    session.browser_session.session_id =
        "pipe-session-" +
        std::to_string(g_next_session_serial.fetch_add(1, std::memory_order_relaxed));
    return session;
}

[[nodiscard]] base::Result<void>
serve_one(adapters::windows_ipc::WindowsNamedPipeListener& listener,
          const ServiceRuntimeInfo& runtime, const ServiceHostOptions& options,
          const base::CancellationToken& cancellation) {
    auto accepted = listener.accept(cancellation);
    if (!accepted) {
        return base::Result<void>::failure(accepted.error());
    }
    const auto session = make_session_context();
    return run_service_session(*accepted.value(), runtime, session, cancellation,
                               options.maximum_requests_per_session);
}

} // namespace

base::Result<void> run_service_host(adapters::windows_ipc::WindowsNamedPipeListener& listener,
                                    const ServiceRuntimeInfo& runtime,
                                    const ServiceHostOptions& options,
                                    const base::CancellationToken& cancellation) {
    if (options.stop_deadline.count() <= 0) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "service stop deadline is invalid"});
    }
    for (;;) {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                base::Error{base::ErrorCode::kCancelled, "service host stop requested"});
        }
        auto served = serve_one(listener, runtime, options, cancellation);
        if (served) {
            if (options.once) {
                return base::Result<void>::success();
            }
            continue;
        }
        if (served.error().code == base::ErrorCode::kCancelled) {
            if (wait_until_stopped(cancellation, options.stop_deadline) ||
                cancellation.stop_requested()) {
                return base::Result<void>::failure(served.error());
            }
            return base::Result<void>::failure(
                base::Error{base::ErrorCode::kInternal, "service host stop deadline exceeded"});
        }
        // Session transport failures end one session without stopping the host, matching
        // interactive forever mode, unless once mode was requested.
        if (!options.once && (served.error().code == base::ErrorCode::kIoFailure ||
                              served.error().code == base::ErrorCode::kInvalidArgument)) {
            continue;
        }
        return base::Result<void>::failure(served.error());
    }
}

} // namespace aegra::apps::service
