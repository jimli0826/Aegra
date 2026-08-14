#pragma once

#include "aegra/apps/service/service_host.h"

namespace aegra::apps::service::detail {

[[nodiscard]] base::Result<void>
run_concurrent_service_session(ports::IMessageChannel& channel, const ServiceRuntimeInfo& runtime,
                               const ServiceSessionContext& session,
                               const base::CancellationToken& cancellation,
                               std::size_t maximum_requests);

} // namespace aegra::apps::service::detail
