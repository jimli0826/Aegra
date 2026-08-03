#pragma once

#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/adapters/windows_ipc/windows_named_pipe_security.h"
#include "aegra/apps/service/service_host.h"
#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace aegra::apps::service {

struct ServiceSecurityHostOptions final {
    adapters::windows_ipc::WindowsServiceCallerAuthorization authorization{};
    std::chrono::milliseconds stop_deadline{std::chrono::milliseconds{5'000}};
    // 0 means unlimited requests per accepted session (until disconnect or cancel).
    std::size_t maximum_requests_per_session{0};
    // When true, accept and serve a single authorized session then return success.
    bool once{false};
};

// Accepts local authorized clients and runs existing V3 sessions. Cancellation stops pending
// accept/receive work. After cancellation is requested the host waits up to stop_deadline for the
// current session to finish, then returns kCancelled if the deadline elapses first.
[[nodiscard]] base::Result<void>
run_authorized_service_host(adapters::windows_ipc::WindowsNamedPipeListener& listener,
                            const ServiceRuntimeInfo& runtime,
                            const ServiceSecurityHostOptions& options,
                            const base::CancellationToken& cancellation);

} // namespace aegra::apps::service
