#include "aegra/apps/service/service_host.h"

#include "aegra/application/connected_repository_query.h"
#include "aegra/application/personal_repository_query.h"
#include "aegra/application/recovery_point_operations.h"
#include "aegra/application/repository_connection_service.h"
#include "aegra/application/source_inventory_query.h"
#include "aegra/apps/service/schedule_service.h"
#include "aegra/apps/service/service_protocol.h"
#include "aegra/apps/service/worker_job_service.h"
#include "aegra/apps/service/worker_supervisor.h"
#include "aegra/contracts/progress.h"
#include "aegra/ports/control_plane.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::apps::service {
namespace {

[[nodiscard]] base::Result<contracts::ServiceResponse>
capability_unavailable(const contracts::ServiceRequest& request);

[[nodiscard]] std::string_view request_kind_name(const contracts::ServiceRequestKind kind) {
    switch (kind) {
    case contracts::ServiceRequestKind::kGetServiceInfo:
        return "service.info";
    case contracts::ServiceRequestKind::kListRecoveryPoints:
        return "repository.list_recovery_points";
    case contracts::ServiceRequestKind::kListRepositoryConnections:
        return "repository.list_connections";
    case contracts::ServiceRequestKind::kListSourceInventory:
        return "source.inventory";
    case contracts::ServiceRequestKind::kListJobs:
        return "job.list";
    case contracts::ServiceRequestKind::kStartBackup:
        return "backup.start";
    case contracts::ServiceRequestKind::kCancelJob:
        return "job.cancel";
    default:
        return "service.request";
    }
}

void write_log(const ServiceRuntimeInfo& runtime, const ServiceLogLevel level,
               const std::string_view message_code, const std::string_view detail) noexcept {
    if (runtime.logger != nullptr) {
        runtime.logger->write(level, message_code, detail);
    }
}

[[nodiscard]] std::string request_detail(const contracts::ServiceRequest& request) {
    std::ostringstream stream;
    stream << "request_id=" << request.request_id
           << " kind=" << request_kind_name(request.kind)
           << " kind_value=" << static_cast<int>(request.kind);
    if (request.idempotency_key) {
        stream << " command=true";
    }
    return stream.str();
}

[[nodiscard]] std::string response_detail(const contracts::ServiceResponse& response) {
    std::ostringstream stream;
    stream << "request_id=" << response.request_id
           << " kind=" << request_kind_name(response.request_kind)
           << " response_kind=" << static_cast<int>(response.kind)
           << " error_code=" << static_cast<int>(response.boundary_error_code)
           << " message_code=" << response.message_code;
    return stream.str();
}

[[nodiscard]] std::string_view required_capability(const contracts::ServiceRequestKind kind) {
    switch (kind) {
    case contracts::ServiceRequestKind::kResolveRecoveryPointChain:
        return "recovery_point.chain";
    case contracts::ServiceRequestKind::kPlanDeleteRecoveryPoints:
    case contracts::ServiceRequestKind::kExecuteDeletePlan:
        return "recovery_point.delete";
    case contracts::ServiceRequestKind::kStartVerify:
        return "recovery_point.verify";
    default:
        return {};
    }
}

[[nodiscard]] bool capability_enabled(const ServiceRuntimeInfo& runtime,
                                      const contracts::ServiceRequestKind kind) {
    const auto required = required_capability(kind);
    return required.empty() || std::ranges::binary_search(runtime.capabilities, required);
}

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
    if (request.kind == contracts::ServiceRequestKind::kListSchedules && runtime.schedules) {
        auto result = runtime.schedules->list_schedules(
            std::get<contracts::ScheduleListRequest>(request.payload), cancellation);
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
               request.kind == contracts::ServiceRequestKind::kStartVerify) {
        handled = true;
        result = runtime.worker_jobs->start_verify(
            std::get<contracts::StartVerifyCommand>(request.payload), *request.idempotency_key,
            cancellation);
    } else if (runtime.recovery_point_operations && request.idempotency_key &&
               request.kind == contracts::ServiceRequestKind::kExecuteDeletePlan) {
        handled = true;
        result = runtime.recovery_point_operations->execute_delete(
            std::get<contracts::ExecuteDeletePlanCommand>(request.payload),
            *request.idempotency_key, cancellation);
    } else if (runtime.worker_jobs && request.idempotency_key &&
               request.kind == contracts::ServiceRequestKind::kCancelJob) {
        handled = true;
        result = runtime.worker_jobs->cancel_job(std::get<contracts::ResourceRef>(request.payload),
                                                 *request.idempotency_key, cancellation);
    } else if (runtime.schedules && request.idempotency_key &&
               request.kind == contracts::ServiceRequestKind::kUpsertSchedule) {
        handled = true;
        result = runtime.schedules->upsert_schedule(
            std::get<contracts::UpsertScheduleCommand>(request.payload), *request.idempotency_key,
            cancellation);
    } else if (runtime.schedules && request.idempotency_key &&
               request.kind == contracts::ServiceRequestKind::kDeleteSchedule) {
        handled = true;
        result = runtime.schedules->delete_schedule(
            std::get<contracts::ResourceRef>(request.payload), *request.idempotency_key,
            cancellation);
    }
    if (!handled)
        return capability_unavailable(request);
    if (!result) {
        // Prefer domain-specific message codes so Desktop can show actionable text.
        std::string message_code = "service.request_failed";
        const auto& detail = result.error().message;
        if (detail.find("repository is unavailable") != std::string::npos) {
            message_code = "backup.repository_unavailable";
        } else if (detail.find("source is not selectable") != std::string::npos) {
            message_code = "backup.source_not_selectable";
        } else if (detail.find("source id not found") != std::string::npos) {
            message_code = "backup.source_not_found";
        } else if (detail.find("worker") != std::string::npos ||
                   detail.find("executable") != std::string::npos) {
            message_code = "backup.worker_unavailable";
        } else if (detail.find("idempotency") != std::string::npos) {
            message_code = "backup.idempotency_conflict";
        } else if (request.kind == contracts::ServiceRequestKind::kStartBackup) {
            message_code = "backup.command_failed";
        }
        return base::Result<contracts::ServiceResponse>::success(
            failure(result.error().code, request.request_id, request.kind, std::move(message_code)));
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
    // Merge live Worker progress into active jobs for Desktop Home Tasks polling.
    if (runtime.worker_supervisor) {
        for (auto& item : result.value().items) {
            if (!item.progress) {
                if (auto live = runtime.worker_supervisor->last_progress(item.job_id)) {
                    item.progress = std::move(*live);
                } else if (item.state == contracts::ServiceJobState::kSucceeded) {
                    // Terminal success without a stored progress snapshot → show 100% in UI.
                    contracts::TaskProgress done;
                    done.schema_version = contracts::kTaskProgressSchemaVersion;
                    done.job_id = item.job_id;
                    done.trace_id = item.trace_id;
                    done.phase = contracts::TaskPhase::kCompleted;
                    done.logical_bytes = 1;
                    done.processed_bytes = 1;
                    done.stored_bytes = 0;
                    done.message_code = "job.succeeded";
                    item.progress = std::move(done);
                }
            }
            // Wire contract requires a non-empty stable message_code on progress payloads.
            if (item.progress && item.progress->message_code.empty()) {
                item.progress->message_code = "job.running";
            }
        }
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

[[nodiscard]] base::Result<contracts::ServiceResponse>
recovery_point_ops_response(const contracts::ServiceRequest& request,
                            const ServiceRuntimeInfo& runtime,
                            const base::CancellationToken cancellation) {
    if (!runtime.recovery_point_operations) {
        return capability_unavailable(request);
    }
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    if (request.kind == contracts::ServiceRequestKind::kResolveRecoveryPointChain) {
        auto result = runtime.recovery_point_operations->resolve_chain(
            std::get<contracts::RecoveryPointRef>(request.payload), cancellation);
        if (!result) {
            return base::Result<contracts::ServiceResponse>::success(
                failure(result.error().code, request.request_id, request.kind,
                        "recovery_point.query_failed"));
        }
        response.message_code = result.value().message_code;
        response.payload = std::move(result).value();
        return base::Result<contracts::ServiceResponse>::success(std::move(response));
    }
    auto result = runtime.recovery_point_operations->plan_delete(
        std::get<contracts::RecoveryPointRef>(request.payload), cancellation);
    if (!result) {
        return base::Result<contracts::ServiceResponse>::success(failure(
            result.error().code, request.request_id, request.kind, "recovery_point.plan_failed"));
    }
    response.message_code = "recovery_point.delete_plan_ready";
    response.payload = std::move(result).value();
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

} // namespace

base::Result<contracts::ServiceResponse>
dispatch_service_request(const contracts::ServiceRequest& request,
                         const ServiceRuntimeInfo& runtime,
                         const base::CancellationToken cancellation) {
    write_log(runtime, ServiceLogLevel::kInfo, "service.request_received",
              request_detail(request));
    auto valid_request = contracts::validate_service_request(request);
    if (!valid_request) {
        write_log(runtime, ServiceLogLevel::kWarning, "service.request_invalid",
                  request_detail(request));
        return base::Result<contracts::ServiceResponse>::failure(valid_request.error());
    }
    if (!capability_enabled(runtime, request.kind)) {
        auto response = capability_unavailable(request);
        if (response) {
            write_log(runtime, ServiceLogLevel::kWarning, "service.request_failed",
                      response_detail(response.value()));
        }
        return response;
    }
    base::Result<contracts::ServiceResponse> response =
        base::Result<contracts::ServiceResponse>::failure(
            {base::ErrorCode::kInvalidArgument, "service request kind is invalid"});
    switch (request.kind) {
    case contracts::ServiceRequestKind::kGetServiceInfo:
        response = service_info_response(request, runtime);
        break;
    case contracts::ServiceRequestKind::kListRecoveryPoints:
        response = recovery_point_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kListJobs:
        response = jobs_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kListRepositoryConnections:
    case contracts::ServiceRequestKind::kListSourceInventory:
        response = query_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kResolveRecoveryPointChain:
    case contracts::ServiceRequestKind::kPlanDeleteRecoveryPoints:
        response = recovery_point_ops_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kAddRepositoryConnection:
    case contracts::ServiceRequestKind::kImportRepositoryConnection:
    case contracts::ServiceRequestKind::kTestRepositoryConnection:
    case contracts::ServiceRequestKind::kSetDefaultRepository:
    case contracts::ServiceRequestKind::kRemoveRepositoryConnection:
    case contracts::ServiceRequestKind::kStartBackup:
    case contracts::ServiceRequestKind::kStartVerify:
    case contracts::ServiceRequestKind::kExecuteDeletePlan:
    case contracts::ServiceRequestKind::kCancelJob:
        response = command_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kListSchedules:
        response = query_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kUpsertSchedule:
    case contracts::ServiceRequestKind::kDeleteSchedule:
        response = command_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kListEvents:
    case contracts::ServiceRequestKind::kListMountSessions:
    case contracts::ServiceRequestKind::kPrepareRestore:
    case contracts::ServiceRequestKind::kStartRestore:
    case contracts::ServiceRequestKind::kMountRecoveryPoint:
    case contracts::ServiceRequestKind::kUnmountSession:
    case contracts::ServiceRequestKind::kSubscribeTaskEvents:
    case contracts::ServiceRequestKind::kAcknowledgeEvents:
        response = capability_unavailable(request);
        break;
    }
    if (!response) {
        write_log(runtime, ServiceLogLevel::kError, "service.dispatch_failed",
                  request_detail(request));
        return response;
    }
    const auto level = response.value().kind == contracts::ServiceResponseKind::kRequestFailed
                           ? ServiceLogLevel::kWarning
                           : ServiceLogLevel::kInfo;
    write_log(runtime, level,
              response.value().kind == contracts::ServiceResponseKind::kRequestFailed
                  ? "service.request_failed"
                  : "service.request_completed",
              response_detail(response.value()));
    return response;
}

base::Result<std::string> handle_service_message(const std::string_view encoded_request,
                                                 const ServiceRuntimeInfo& runtime,
                                                 const base::CancellationToken cancellation) {
    auto request = decode_service_request(encoded_request);
    if (!request) {
        const auto code = request.error().code == base::ErrorCode::kUnsupportedVersion
                              ? base::ErrorCode::kUnsupportedVersion
                              : base::ErrorCode::kInvalidArgument;
        std::ostringstream detail;
        detail << "error_code=" << static_cast<int>(code);
        write_log(runtime, ServiceLogLevel::kWarning, "service.request_decode_failed",
                  detail.str());
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
