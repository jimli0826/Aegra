#include "aegra/apps/service/service_host.h"

#include "aegra/apps/service/service_protocol.h"

#include <algorithm>
#include <utility>

namespace aegra::apps::service {
namespace {

[[nodiscard]] contracts::ServiceResponse rejection(const base::ErrorCode code) {
    contracts::ServiceResponse response;
    response.kind = contracts::ServiceResponseKind::kRequestRejected;
    response.boundary_error_code = code == base::ErrorCode::kUnsupportedVersion
                                       ? base::ErrorCode::kUnsupportedVersion
                                       : base::ErrorCode::kInvalidArgument;
    response.message_code = "service.request_rejected";
    return response;
}

[[nodiscard]] contracts::ServiceInfo make_service_info(const ServiceRuntimeInfo& runtime) {
    contracts::ServiceInfo service;
    service.state = contracts::ServiceState::kReady;
    service.service_version = runtime.service_version;
    service.capabilities = runtime.capabilities;
    return service;
}

} // namespace

base::Result<contracts::ServiceResponse>
dispatch_service_request(const contracts::ServiceRequest& request,
                         const ServiceRuntimeInfo& runtime) {
    auto valid_request = contracts::validate_service_request(request);
    if (!valid_request) {
        return base::Result<contracts::ServiceResponse>::failure(valid_request.error());
    }
    auto service = make_service_info(runtime);
    auto valid_service = contracts::validate_service_info(service);
    if (!valid_service) {
        return base::Result<contracts::ServiceResponse>::failure(valid_service.error());
    }
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kServiceInfo;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "service.ready";
    response.service = std::move(service);
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

base::Result<std::string> handle_service_message(const std::string_view encoded_request,
                                                 const ServiceRuntimeInfo& runtime) {
    auto request = decode_service_request(encoded_request);
    if (!request) {
        return encode_service_response(rejection(request.error().code));
    }
    auto response = dispatch_service_request(request.value(), runtime);
    if (!response) {
        return base::Result<std::string>::failure(response.error());
    }
    return encode_service_response(response.value());
}

base::Result<void> run_service_session(ports::IMessageChannel& channel,
                                       const ServiceRuntimeInfo& runtime,
                                       const base::CancellationToken& cancellation,
                                       const std::size_t maximum_requests) {
    std::size_t processed = 0;
    while (maximum_requests == 0 || processed < maximum_requests) {
        auto request = channel.receive(cancellation);
        if (!request) {
            return base::Result<void>::failure(request.error());
        }
        auto response = handle_service_message(request.value(), runtime);
        if (!response) {
            return base::Result<void>::failure(response.error());
        }
        auto sent = channel.send(response.value(), cancellation);
        if (!sent) {
            return sent;
        }
        ++processed;
    }
    return base::Result<void>::success();
}

} // namespace aegra::apps::service
