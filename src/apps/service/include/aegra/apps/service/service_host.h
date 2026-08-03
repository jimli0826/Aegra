#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service.h"
#include "aegra/ports/message_channel.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::apps::service {

struct ServiceRuntimeInfo final {
    std::string service_version;
    std::vector<std::string> capabilities;
};

[[nodiscard]] base::Result<contracts::ServiceResponse>
dispatch_service_request(const contracts::ServiceRequest& request,
                         const ServiceRuntimeInfo& runtime);

[[nodiscard]] base::Result<std::string> handle_service_message(std::string_view encoded_request,
                                                               const ServiceRuntimeInfo& runtime);

[[nodiscard]] base::Result<void> run_service_session(ports::IMessageChannel& channel,
                                                     const ServiceRuntimeInfo& runtime,
                                                     const base::CancellationToken& cancellation,
                                                     std::size_t maximum_requests = 0);

} // namespace aegra::apps::service
