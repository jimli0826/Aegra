#include "aegra/apps/service/service_host.h"

#include "aegra/application/personal_repository_query.h"
#include "aegra/apps/service/service_protocol.h"

#include <algorithm>
#include <utility>

namespace aegra::apps::service {
namespace {

[[nodiscard]] contracts::ServiceResponse
failure(const base::ErrorCode code, std::string request_id = {},
        std::string message_code = "service.request_failed") {
    contracts::ServiceResponse response;
    response.request_id = std::move(request_id);
    response.kind = contracts::ServiceResponseKind::kRequestFailed;
    response.boundary_error_code =
        code == base::ErrorCode::kNone ? base::ErrorCode::kInternal : code;
    response.message_code = std::move(message_code);
    return response;
}

[[nodiscard]] contracts::ServiceInfo make_service_info(const ServiceRuntimeInfo& runtime) {
    contracts::ServiceInfo service;
    service.state = contracts::ServiceState::kReady;
    service.service_version = runtime.service_version;
    service.capabilities = runtime.capabilities;
    return service;
}

[[nodiscard]] base::Result<contracts::ServiceResponse>
service_info_response(const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime) {
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

[[nodiscard]] base::Result<contracts::ServiceResponse>
recovery_point_response(const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime,
                        const base::CancellationToken cancellation) {
    contracts::RecoveryPointPage page;
    if (runtime.repository_query != nullptr) {
        auto queried =
            runtime.repository_query->list_recovery_points(*request.repository_list, cancellation);
        if (!queried) {
            return base::Result<contracts::ServiceResponse>::success(
                failure(queried.error().code, request.request_id, "repository.query_failed"));
        }
        page = std::move(queried).value();
    }
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kRecoveryPointPage;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = page.state == contracts::RepositoryCatalogState::kNotConfigured
                                ? "repository.not_configured"
                                : "repository.catalog_ready";
    response.recovery_points = std::move(page);
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

} // namespace

base::Result<contracts::ServiceResponse>
dispatch_service_request(const contracts::ServiceRequest& request,
                         const ServiceRuntimeInfo& runtime,
                         const base::CancellationToken cancellation) {
    auto valid_request = contracts::validate_service_request(request);
    if (!valid_request) {
        return base::Result<contracts::ServiceResponse>::failure(valid_request.error());
    }
    switch (request.kind) {
    case contracts::ServiceRequestKind::kGetServiceInfo:
        return service_info_response(request, runtime);
    case contracts::ServiceRequestKind::kListRecoveryPoints:
        return recovery_point_response(request, runtime, cancellation);
    }
    return base::Result<contracts::ServiceResponse>::failure(
        {base::ErrorCode::kInvalidArgument, "service request kind is invalid"});
}

base::Result<std::string> handle_service_message(const std::string_view encoded_request,
                                                 const ServiceRuntimeInfo& runtime,
                                                 const base::CancellationToken cancellation) {
    auto request = decode_service_request(encoded_request);
    if (!request) {
        const auto code = request.error().code == base::ErrorCode::kUnsupportedVersion
                              ? base::ErrorCode::kUnsupportedVersion
                              : base::ErrorCode::kInvalidArgument;
        return encode_service_response(failure(code));
    }
    auto response = dispatch_service_request(request.value(), runtime, cancellation);
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
        auto response = handle_service_message(request.value(), runtime, cancellation);
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
