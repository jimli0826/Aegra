#pragma once

#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/apps/service/service_host.h"
#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace aegra::apps::service {

struct ServiceHostOptions final {
    std::chrono::milliseconds stop_deadline{std::chrono::milliseconds{5'000}};
    // 0 means unlimited requests per accepted session (until disconnect or cancel).
    std::size_t maximum_requests_per_session{0};
    // When true, accept and serve a single local session then return success.
    bool once{false};
};

// Accepts local clients and runs Service sessions. The pipe ACL/local-only transport is the only
// connection boundary; the Service does not authenticate or authorize a caller identity.
// Cancellation stops pending
// accept/receive work. After cancellation is requested the host waits up to stop_deadline for the
// current session to finish, then returns kCancelled if the deadline elapses first.
[[nodiscard]] base::Result<void>
run_service_host(adapters::windows_ipc::WindowsNamedPipeListener& listener,
                 const ServiceRuntimeInfo& runtime, const ServiceHostOptions& options,
                 const base::CancellationToken& cancellation);

} // namespace aegra::apps::service
