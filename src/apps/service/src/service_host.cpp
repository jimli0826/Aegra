#include "aegra/apps/service/service_host.h"

#include "aegra/application/connected_repository_query.h"
#include "aegra/application/personal_repository_query.h"
#include "aegra/application/repository_connection_service.h"
#include "aegra/application/source_inventory_query.h"
#include "aegra/apps/service/service_protocol.h"
#include "aegra/apps/service/worker_job_service.h"
#include "aegra/ports/control_plane.h"

#include <algorithm>
#include <string>
#include <utility>

namespace aegra::apps::service {
namespace {

[[nodiscard]] base::Result<contracts::ServiceResponse>
capability_unavailable(const contracts::ServiceRequest& request);

[[nodiscard]] contracts::ServiceResponse
failure(const base::ErrorCode code, std::string request_id = {},
        const contracts::ServiceRequestKind request_kind =
            contracts::ServiceRequestKind::kGetServiceInfo,
        std::string message_code = "service.request_failed",
        contracts::MessageArguments message_arguments = {}) {
    contracts::ServiceResponse response;
    response.request_id = std::move(request_id);
    response.kind = contracts::ServiceResponseKind::kRequestFailed;
    response.request_kind = request_kind;
    response.boundary_error_code =
        code == base::ErrorCode::kNone ? base::ErrorCode::kInternal : code;
    response.message_code = std::move(message_code);
    response.message_arguments = std::move(message_arguments);
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
    const auto& requested = std::get<contracts::ServiceVersionRange>(request.payload);
    if (requested.minimum_api_version > contracts::kServiceApiVersion ||
        requested.maximum_api_version < contracts::kServiceApiVersion) {
        return base::Result<contracts::ServiceResponse>::success(
            failure(base::ErrorCode::kUnsupportedVersion, request.request_id, request.kind,
                    "service.api_version_unsupported",
                    {{"maximum_api_version", std::to_string(requested.maximum_api_version)},
                     {"minimum_api_version", std::to_string(requested.minimum_api_version)},
                     {"service_api_version", std::to_string(contracts::kServiceApiVersion)}}));
    }
    auto service = make_service_info(runtime);
    auto valid_service = contracts::validate_service_info(service);
    if (!valid_service) {
        return base::Result<contracts::ServiceResponse>::failure(valid_service.error());
    }
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "service.ready";
    response.payload = std::move(service);
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

[[nodiscard]] base::Result<contracts::ServiceResponse>
recovery_point_response(const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime,
                        const base::CancellationToken cancellation) {
    contracts::RecoveryPointPage page;
    const auto& query = std::get<contracts::ServiceRecoveryPointListRequest>(request.payload);
    if (runtime.repository_query != nullptr && !query.repository_connection_id) {
        auto queried = runtime.repository_query->list_recovery_points(query.page, cancellation);
        if (!queried) {
            return base::Result<contracts::ServiceResponse>::success(failure(
                queried.error().code, request.request_id, request.kind, "repository.query_failed"));
        }
        page = std::move(queried).value();
    }
    if (runtime.connected_repository_query != nullptr &&
        (runtime.repository_query == nullptr || query.repository_connection_id)) {
        auto queried =
            runtime.connected_repository_query->list_recovery_points(query, cancellation);
        if (!queried) {
            return base::Result<contracts::ServiceResponse>::success(failure(
                queried.error().code, request.request_id, request.kind, "repository.query_failed"));
        }
        contracts::ServiceResponse response;
        response.request_id = request.request_id;
        response.kind = contracts::ServiceResponseKind::kQueryResult;
        response.request_kind = request.kind;
        response.boundary_error_code = base::ErrorCode::kNone;
        response.message_code = "repository.catalog_ready";
        response.payload = std::move(queried).value();
        return base::Result<contracts::ServiceResponse>::success(std::move(response));
    }
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = page.state == contracts::RepositoryCatalogState::kNotConfigured
                                ? "repository.not_configured"
                                : "repository.catalog_ready";
    response.payload =
        contracts::ServiceRecoveryPointPage{query.repository_connection_id, std::move(page)};
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

[[nodiscard]] base::Result<contracts::ServiceResponse>
query_response(const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime,
               const base::CancellationToken cancellation) {
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "control_plane.ready";
    if (request.kind == contracts::ServiceRequestKind::kListRepositoryConnections &&
        runtime.repository_connections) {
        auto result = runtime.repository_connections->list_connections(
            std::get<contracts::RepositoryConnectionListRequest>(request.payload), cancellation);
        if (!result)
            return base::Result<contracts::ServiceResponse>::success(
                failure(result.error().code, request.request_id, request.kind));
        response.payload = std::move(result).value();
        return base::Result<contracts::ServiceResponse>::success(std::move(response));
    }
    if (request.kind == contracts::ServiceRequestKind::kListSourceInventory &&
        runtime.source_inventory) {
        auto result = runtime.source_inventory->list_sources(
            std::get<contracts::SourceInventoryListRequest>(request.payload), cancellation);
        if (!result)
            return base::Result<contracts::ServiceResponse>::success(
                failure(result.error().code, request.request_id, request.kind));
        response.payload = std::move(result).value();
        return base::Result<contracts::ServiceResponse>::success(std::move(response));
    }
    return capability_unavailable(request);
}

[[nodiscard]] base::Result<contracts::CommandAcknowledgement>
run_repository_command(const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime,
                       const base::CancellationToken cancellation) {
    if (!runtime.repository_connections || !request.idempotency_key) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "repository command unavailable"});
    }
    const auto key = *request.idempotency_key;
    switch (request.kind) {
    case contracts::ServiceRequestKind::kAddRepositoryConnection:
        return runtime.repository_connections->add_connection(
            std::get<contracts::RepositoryConnectionInput>(request.payload), key, cancellation);
    case contracts::ServiceRequestKind::kImportRepositoryConnection:
        return runtime.repository_connections->import_connection(
            std::get<contracts::RepositoryConnectionInput>(request.payload), key, cancellation);
    case contracts::ServiceRequestKind::kTestRepositoryConnection:
        return runtime.repository_connections->test_connection(
            std::get<contracts::ResourceRef>(request.payload), key, cancellation);
    case contracts::ServiceRequestKind::kSetDefaultRepository:
        return runtime.repository_connections->set_default_connection(
            std::get<contracts::ResourceRef>(request.payload), key, cancellation);
    case contracts::ServiceRequestKind::kRemoveRepositoryConnection:
        return runtime.repository_connections->remove_connection(
            std::get<contracts::ResourceRef>(request.payload), key, cancellation);
    default:
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kInvalidArgument, "repository command kind is invalid"});
    }
}

[[nodiscard]] base::Result<contracts::ServiceResponse>
command_response(const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime,
                 const base::CancellationToken cancellation) {
    base::Result<contracts::CommandAcknowledgement> result =
        base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "command unavailable"});
    bool handled = false;
    if (request.kind >= contracts::ServiceRequestKind::kAddRepositoryConnection &&
        request.kind <= contracts::ServiceRequestKind::kRemoveRepositoryConnection) {
        handled = runtime.repository_connections != nullptr;
        result = run_repository_command(request, runtime, cancellation);
    } else if (runtime.worker_jobs && request.idempotency_key &&
               request.kind == contracts::ServiceRequestKind::kStartBackup) {
        handled = true;
        result = runtime.worker_jobs->start_backup(
            std::get<contracts::StartBackupCommand>(request.payload), *request.idempotency_key,
            cancellation);
    } else if (runtime.worker_jobs && request.idempotency_key &&
               request.kind == contracts::ServiceRequestKind::kCancelJob) {
        handled = true;
        result = runtime.worker_jobs->cancel_job(std::get<contracts::ResourceRef>(request.payload),
                                                 *request.idempotency_key, cancellation);
    }
    if (!handled)
        return capability_unavailable(request);
    if (!result) {
        return base::Result<contracts::ServiceResponse>::success(
            failure(result.error().code, request.request_id, request.kind));
    }
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kCommandAccepted;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = result.value().disposition == contracts::CommandDisposition::kReplayed
                                ? "command.replayed"
                                : "command.accepted";
    response.payload = std::move(result).value();
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

[[nodiscard]] base::Result<contracts::ServiceResponse>
capability_unavailable(const contracts::ServiceRequest& request) {
    return base::Result<contracts::ServiceResponse>::success(
        failure(base::ErrorCode::kConflict, request.request_id, request.kind,
                "service.capability_unavailable",
                {{"request_kind", std::to_string(static_cast<std::uint8_t>(request.kind))}}));
}

[[nodiscard]] base::Result<contracts::ServiceResponse>
jobs_response(const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime,
              const base::CancellationToken cancellation) {
    if (!runtime.control_plane) {
        return capability_unavailable(request);
    }
    const auto& query = std::get<contracts::JobListRequest>(request.payload);
    auto result = runtime.control_plane->list_jobs(query, cancellation);
    if (!result) {
        return base::Result<contracts::ServiceResponse>::success(failure(
            result.error().code, request.request_id, request.kind, "control_plane.query_failed"));
    }
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "control_plane.ready";
    response.payload = std::move(result).value();
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
    case contracts::ServiceRequestKind::kListJobs:
        return jobs_response(request, runtime, cancellation);
    case contracts::ServiceRequestKind::kListRepositoryConnections:
    case contracts::ServiceRequestKind::kListSourceInventory:
        return query_response(request, runtime, cancellation);
    case contracts::ServiceRequestKind::kAddRepositoryConnection:
    case contracts::ServiceRequestKind::kImportRepositoryConnection:
    case contracts::ServiceRequestKind::kTestRepositoryConnection:
    case contracts::ServiceRequestKind::kSetDefaultRepository:
    case contracts::ServiceRequestKind::kRemoveRepositoryConnection:
    case contracts::ServiceRequestKind::kStartBackup:
    case contracts::ServiceRequestKind::kCancelJob:
        return command_response(request, runtime, cancellation);
    case contracts::ServiceRequestKind::kListSchedules:
    case contracts::ServiceRequestKind::kListEvents:
    case contracts::ServiceRequestKind::kListMountSessions:
    case contracts::ServiceRequestKind::kPrepareRestore:
    case contracts::ServiceRequestKind::kStartVerify:
    case contracts::ServiceRequestKind::kStartRestore:
    case contracts::ServiceRequestKind::kMountRecoveryPoint:
    case contracts::ServiceRequestKind::kUnmountSession:
    case contracts::ServiceRequestKind::kUpsertSchedule:
    case contracts::ServiceRequestKind::kDeleteSchedule:
    case contracts::ServiceRequestKind::kSubscribeTaskEvents:
    case contracts::ServiceRequestKind::kAcknowledgeEvents:
        return capability_unavailable(request);
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
