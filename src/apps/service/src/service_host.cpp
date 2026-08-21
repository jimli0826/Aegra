#include "aegra/apps/service/service_host.h"

#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/application/connected_repository_query.h"
#include "aegra/application/file_browse_service.h"
#include "aegra/application/personal_repository_query.h"
#include "aegra/application/recovery_point_operations.h"
#include "aegra/application/repository_connection_service.h"
#include "aegra/application/source_inventory_query.h"
#include "aegra/apps/service/mount_supervisor.h"
#include "aegra/apps/service/schedule_service.h"
#include "aegra/apps/service/service_protocol.h"
#include "aegra/apps/service/service_response_fit.h"
#include "aegra/apps/service/worker_job_service.h"
#include "aegra/apps/service/worker_supervisor.h"
#include "aegra/ports/control_plane.h"
#include "file_recovery_point_query.h"
#include "network_share_access.h"
#include "recovery_point_layout_service.h"
#include "service_log_formatter.h"
#include "service_session_executor.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::apps::service {
namespace {

[[nodiscard]] base::Result<contracts::ServiceResponse>
capability_unavailable(const contracts::ServiceRequest& request);

void write_log(const ServiceRuntimeInfo& runtime, const ServiceLogLevel level,
               const std::string_view message_code, const std::string_view detail) noexcept {
    if (runtime.logger != nullptr) {
        runtime.logger->write(level, message_code, detail);
    }
}

[[nodiscard]] std::string request_detail(const contracts::ServiceRequest& request) {
    return detail::request_log_detail(request);
}

[[nodiscard]] std::string response_detail(const contracts::ServiceResponse& response) {
    return detail::response_log_detail(response);
}

void write_interaction_log(const ServiceRuntimeInfo& runtime, const std::string_view direction,
                           const std::string_view encoded) noexcept {
    const auto message_code =
        direction == "Inbound" ? "service.interaction.request" : "service.interaction.response";
    write_log(runtime, ServiceLogLevel::kTrace, message_code,
              detail::sanitized_interaction_detail(direction, encoded));
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
    case contracts::ServiceRequestKind::kListRecoveryPointEntries:
        return "file.recover_browse";
    case contracts::ServiceRequestKind::kPrepareFileRestore:
    case contracts::ServiceRequestKind::kStartFileRestore:
        return "file.restore";
    case contracts::ServiceRequestKind::kGetServiceSettings:
    case contracts::ServiceRequestKind::kUpdateServiceSettings:
        return "service.settings";
    case contracts::ServiceRequestKind::kAnalyzeNtfsShrink:
        return "restore.ntfs_shrink.v1";
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
    const auto& query = std::get<contracts::ServiceRecoveryPointListRequest>(request.payload);
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "repository.catalog_ready";

    const auto fetch = [&](const std::uint32_t maximum_results)
        -> base::Result<contracts::ServiceRecoveryPointPage> {
        auto scaled = query;
        scaled.page.maximum_results = maximum_results;
        if (runtime.connected_repository_query != nullptr &&
            (runtime.repository_query == nullptr || scaled.repository_connection_id)) {
            return runtime.connected_repository_query->list_recovery_points(scaled, cancellation);
        }
        if (runtime.repository_query != nullptr && !scaled.repository_connection_id) {
            auto queried =
                runtime.repository_query->list_recovery_points(scaled.page, cancellation);
            if (!queried) {
                return base::Result<contracts::ServiceRecoveryPointPage>::failure(queried.error());
            }
            return base::Result<contracts::ServiceRecoveryPointPage>::success(
                contracts::ServiceRecoveryPointPage{scaled.repository_connection_id,
                                                    std::move(queried).value()});
        }
        return base::Result<contracts::ServiceRecoveryPointPage>::failure(
            {base::ErrorCode::kConflict, "repository query unavailable"});
    };

    auto fitted = fetch_payload_within_frame_budget<contracts::ServiceRecoveryPointPage>(
        response, query.page.maximum_results, fetch,
        [](const contracts::ServiceRecoveryPointPage& page) { return page.catalog.items.size(); });
    if (!fitted) {
        return base::Result<contracts::ServiceResponse>::success(failure(
            fitted.error().code, request.request_id, request.kind, "repository.query_failed"));
    }
    if (fitted.value().catalog.state == contracts::RepositoryCatalogState::kNotConfigured) {
        response.message_code = "repository.not_configured";
    }
    response.payload = std::move(fitted).value();
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
        const auto& query = std::get<contracts::RepositoryConnectionListRequest>(request.payload);
        auto fitted = fetch_page_within_frame_budget<contracts::RepositoryConnectionPage>(
            response, query.page.maximum_results, [&](const std::uint32_t maximum_results) {
                auto scaled = query;
                scaled.page.maximum_results = maximum_results;
                return runtime.repository_connections->list_connections(scaled, cancellation);
            });
        if (!fitted) {
            return base::Result<contracts::ServiceResponse>::success(
                failure(fitted.error().code, request.request_id, request.kind));
        }
        response.payload = std::move(fitted).value();
        return base::Result<contracts::ServiceResponse>::success(std::move(response));
    }
    if (request.kind == contracts::ServiceRequestKind::kListSourceInventory &&
        runtime.source_inventory) {
        const auto& query = std::get<contracts::SourceInventoryListRequest>(request.payload);
        auto fitted = fetch_page_within_frame_budget<contracts::SourceInventoryPage>(
            response, query.page.maximum_results, [&](const std::uint32_t maximum_results) {
                auto scaled = query;
                scaled.page.maximum_results = maximum_results;
                return runtime.source_inventory->list_sources(scaled, cancellation);
            });
        if (!fitted) {
            return base::Result<contracts::ServiceResponse>::success(
                failure(fitted.error().code, request.request_id, request.kind));
        }
        response.payload = std::move(fitted).value();
        return base::Result<contracts::ServiceResponse>::success(std::move(response));
    }
    if (request.kind == contracts::ServiceRequestKind::kListSchedules && runtime.schedules) {
        const auto& query = std::get<contracts::ScheduleListRequest>(request.payload);
        auto fitted = fetch_page_within_frame_budget<contracts::SchedulePage>(
            response, query.page.maximum_results, [&](const std::uint32_t maximum_results) {
                auto scaled = query;
                scaled.page.maximum_results = maximum_results;
                return runtime.schedules->list_schedules(scaled, cancellation);
            });
        if (!fitted) {
            return base::Result<contracts::ServiceResponse>::success(
                failure(fitted.error().code, request.request_id, request.kind));
        }
        response.payload = std::move(fitted).value();
        return base::Result<contracts::ServiceResponse>::success(std::move(response));
    }
    if (request.kind == contracts::ServiceRequestKind::kPrepareRestore && runtime.worker_jobs) {
        auto result = runtime.worker_jobs->prepare_restore(
            std::get<contracts::RestorePreflightRequest>(request.payload), cancellation);
        if (!result) {
            write_log(runtime, ServiceLogLevel::kWarning, "restore.preflight_failed",
                      result.error().message.empty() ? "prepare restore failed"
                                                     : result.error().message);
            return base::Result<contracts::ServiceResponse>::success(failure(
                result.error().code, request.request_id, request.kind, "restore.preflight_failed"));
        }
        response.message_code = result.value().message_code;
        response.payload = std::move(result).value();
        return base::Result<contracts::ServiceResponse>::success(std::move(response));
    }
    if (request.kind == contracts::ServiceRequestKind::kAnalyzeNtfsShrink && runtime.worker_jobs) {
        auto result = runtime.worker_jobs->analyze_ntfs_shrink(
            std::get<contracts::RestorePreflightRequest>(request.payload), cancellation);
        if (!result) {
            const auto message_code =
                result.error().message.starts_with("restore.shrink_") ||
                        result.error().message.starts_with("ntfs_resize.")
                    ? result.error().message
                    : "restore.shrink_analyze_failed";
            write_log(runtime, ServiceLogLevel::kWarning, message_code,
                      result.error().message.empty() ? "analyze ntfs shrink failed"
                                                     : result.error().message);
            return base::Result<contracts::ServiceResponse>::success(
                failure(result.error().code, request.request_id, request.kind, message_code));
        }
        response.message_code = result.value().message_code;
        response.payload = std::move(result).value();
        return base::Result<contracts::ServiceResponse>::success(std::move(response));
    }
    return capability_unavailable(request);
}

[[nodiscard]] std::string network_entropy_id_for_locator(const std::string_view locator) {
    std::string entropy = "net.";
    for (const unsigned char character : locator) {
        if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
            character == '.' || character == '_' || character == '-') {
            entropy.push_back(static_cast<char>(character));
        } else if (character >= 'A' && character <= 'Z') {
            entropy.push_back(static_cast<char>(character - 'A' + 'a'));
        } else {
            entropy.push_back('_');
        }
        if (entropy.size() >= 96U) {
            break;
        }
    }
    return entropy;
}

/// Connect UNC share when needed and DPAPI-protect network auth into credential_ref.
[[nodiscard]] base::Result<contracts::RepositoryConnectionInput>
prepare_repository_connection_input(contracts::RepositoryConnectionInput input,
                                    const base::CancellationToken cancellation) {
    const bool has_network_auth = !input.network_username.empty() ||
                                  !input.network_password.empty() || !input.network_domain.empty();
    if (has_network_auth && !is_unc_locator(input.locator)) {
        return base::Result<contracts::RepositoryConnectionInput>::failure(
            {base::ErrorCode::kInvalidArgument, "repository.network_path_invalid"});
    }
    if (is_unc_locator(input.locator)) {
        auto reachable = probe_network_share_server(input.locator, cancellation);
        if (!reachable) {
            return base::Result<contracts::RepositoryConnectionInput>::failure(reachable.error());
        }
        auto connected =
            connect_network_share(input.locator, input.network_username, input.network_password,
                                  input.network_domain, cancellation);
        if (!connected) {
            return base::Result<contracts::RepositoryConnectionInput>::failure(connected.error());
        }
        if (has_network_auth) {
            const auto packed = pack_network_auth_material(
                input.network_username, input.network_domain, input.network_password);
            const auto entropy = network_entropy_id_for_locator(input.locator);
            auto protected_secret =
                adapters::windows_system::protect_local_machine_secret_blob(packed, entropy);
            if (!input.network_password.empty()) {
                SecureZeroMemory(input.network_password.data(), input.network_password.size());
            }
            if (!protected_secret) {
                return base::Result<contracts::RepositoryConnectionInput>::failure(
                    protected_secret.error());
            }
            input.credential_ref = std::move(protected_secret).value();
        }
    }
    input.network_password.clear();
    return base::Result<contracts::RepositoryConnectionInput>::success(std::move(input));
}

/// Validate and connect a UNC share without protecting credentials or writing control-plane state.
[[nodiscard]] base::Result<void>
connect_repository_location(const contracts::RepositoryConnectionInput& input,
                            const base::CancellationToken cancellation) {
    if (!is_unc_locator(input.locator)) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "repository.network_path_invalid"});
    }
    // TCP reachability first: only this path may report network_unreachable.
    auto reachable = probe_network_share_server(input.locator, cancellation);
    if (!reachable) {
        return reachable;
    }
    auto connected = connect_network_share(input.locator, input.network_username,
                                           input.network_password, input.network_domain,
                                           cancellation);
    if (!connected) {
        // Host already answered on 445. Do not surface auth/provider failures as "unreachable".
        const auto& detail = connected.error().message;
        if (detail == "repository.network_unreachable") {
            const bool has_auth =
                !input.network_username.empty() || !input.network_password.empty();
            return base::Result<void>::failure(
                {has_auth ? base::ErrorCode::kUnauthorized : base::ErrorCode::kIoFailure,
                 has_auth ? "repository.network_credentials_rejected"
                          : "repository.network_connect_failed"});
        }
    }
    return connected;
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
    case contracts::ServiceRequestKind::kAddRepositoryConnection: {
        auto prepared = prepare_repository_connection_input(
            std::get<contracts::RepositoryConnectionInput>(request.payload), cancellation);
        if (!prepared) {
            return base::Result<contracts::CommandAcknowledgement>::failure(prepared.error());
        }
        return runtime.repository_connections->add_connection(prepared.value(), key, cancellation);
    }
    case contracts::ServiceRequestKind::kImportRepositoryConnection: {
        auto prepared = prepare_repository_connection_input(
            std::get<contracts::RepositoryConnectionInput>(request.payload), cancellation);
        if (!prepared) {
            return base::Result<contracts::CommandAcknowledgement>::failure(prepared.error());
        }
        return runtime.repository_connections->import_connection(prepared.value(), key,
                                                                 cancellation);
    }
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

[[nodiscard]] std::string repository_failure_message_code(const base::Error& error) {
    const auto& detail = error.message;
    if (detail.rfind("repository.", 0) == 0) {
        return detail;
    }
    if (detail.find("repository locator already registered") != std::string::npos) {
        return "repository.locator_exists";
    }
    if (detail.find("repository root is not empty") != std::string::npos ||
        detail.find("repository root is not an empty directory") != std::string::npos) {
        return "repository.location_occupied";
    }
    if (detail.find("local storage root is invalid") != std::string::npos) {
        return "repository.storage_root_invalid";
    }
    switch (error.code) {
    case base::ErrorCode::kUnauthorized:
        return "repository.storage_access_denied";
    case base::ErrorCode::kNotFound:
        return "repository.storage_path_not_found";
    case base::ErrorCode::kCorruptData:
        return "repository.descriptor_invalid";
    case base::ErrorCode::kIoFailure:
        return "repository.storage_io_failed";
    default:
        return "service.request_failed";
    }
}

[[nodiscard]] base::Result<contracts::ServiceResponse>
command_response(const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime,
                 const ServiceSessionContext& session, const base::CancellationToken cancellation) {
    base::Result<contracts::CommandAcknowledgement> result =
        base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "command unavailable"});
    bool handled = false;
    if (request.kind == contracts::ServiceRequestKind::kConnectRepositoryLocation &&
        request.idempotency_key && runtime.repository_location_browse != nullptr) {
        auto connected = connect_repository_location(
            std::get<contracts::RepositoryConnectionInput>(request.payload), cancellation);
        if (!connected) {
            handled = true;
            result = base::Result<contracts::CommandAcknowledgement>::failure(connected.error());
        } else {
            auto registered = runtime.repository_location_browse->register_location(
                session.browser_session.session_id, *request.idempotency_key,
                std::get<contracts::RepositoryConnectionInput>(request.payload).locator);
            handled = true;
            if (!registered) {
                result =
                    base::Result<contracts::CommandAcknowledgement>::failure(registered.error());
            } else {
                contracts::CommandAcknowledgement acknowledgement;
                acknowledgement.command_id = *request.idempotency_key;
                acknowledgement.disposition = contracts::CommandDisposition::kAccepted;
                acknowledgement.resource_id = *request.idempotency_key;
                result = base::Result<contracts::CommandAcknowledgement>::success(
                    std::move(acknowledgement));
            }
        }
    } else if (request.kind >= contracts::ServiceRequestKind::kAddRepositoryConnection &&
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
               request.kind == contracts::ServiceRequestKind::kStartRestore) {
        handled = true;
        result = runtime.worker_jobs->start_restore(
            std::get<contracts::StartRestoreCommand>(request.payload), *request.idempotency_key,
            cancellation);
    } else if (runtime.worker_jobs && request.idempotency_key &&
               request.kind == contracts::ServiceRequestKind::kStartFileRestore) {
        handled = true;
        result = runtime.worker_jobs->start_file_restore(
            std::get<contracts::StartFileRestoreCommand>(request.payload), *request.idempotency_key,
            cancellation);
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
            session.browser_session, cancellation);
    } else if (runtime.schedules && request.idempotency_key &&
               request.kind == contracts::ServiceRequestKind::kDeleteSchedule) {
        handled = true;
        result =
            runtime.schedules->delete_schedule(std::get<contracts::ResourceRef>(request.payload),
                                               *request.idempotency_key, cancellation);
    } else if (runtime.mount_supervisor && request.idempotency_key &&
               request.kind == contracts::ServiceRequestKind::kMountRecoveryPoint) {
        handled = true;
        result = runtime.mount_supervisor->mount(
            std::get<contracts::MountRecoveryPointCommand>(request.payload),
            *request.idempotency_key, cancellation);
    } else if (runtime.mount_supervisor && request.idempotency_key &&
               request.kind == contracts::ServiceRequestKind::kUnmountSession) {
        handled = true;
        result =
            runtime.mount_supervisor->unmount(std::get<contracts::ResourceRef>(request.payload),
                                              *request.idempotency_key, cancellation);
    }
    if (!handled)
        return capability_unavailable(request);
    if (!result) {
        // Prefer domain-specific message codes so Desktop can show actionable text.
        std::string message_code = "service.request_failed";
        const auto& detail = result.error().message;
        if ((request.kind >= contracts::ServiceRequestKind::kAddRepositoryConnection &&
             request.kind <= contracts::ServiceRequestKind::kRemoveRepositoryConnection) ||
            request.kind == contracts::ServiceRequestKind::kConnectRepositoryLocation) {
            message_code = repository_failure_message_code(result.error());
        } else if (detail.rfind("mount.", 0) == 0) {
            message_code = detail;
        } else if (detail.find("repository is unavailable") != std::string::npos) {
            message_code = "backup.repository_unavailable";
        } else if (detail.find("source is not selectable") != std::string::npos) {
            message_code = "backup.source_not_selectable";
        } else if (detail.find("source id not found") != std::string::npos) {
            message_code = "backup.source_not_found";
        } else if (detail.find("no eligible parent") != std::string::npos ||
                   detail.find("parent recovery point") != std::string::npos ||
                   detail.find("cannot be used for incremental") != std::string::npos) {
            message_code = "backup.parent_unavailable";
        } else if (detail.find("worker") != std::string::npos ||
                   detail.find("executable") != std::string::npos) {
            message_code = "backup.worker_unavailable";
        } else if (detail.find("idempotency") != std::string::npos) {
            message_code = "backup.idempotency_conflict";
        } else if (detail.find("preflight") != std::string::npos) {
            message_code = "restore.preflight_invalid";
        } else if (detail.find("system disk restore") != std::string::npos) {
            message_code = "restore.system_target_requires_pe";
        } else if (detail.find("restore target is smaller") != std::string::npos) {
            message_code = "restore.target_too_small";
        } else if (request.kind == contracts::ServiceRequestKind::kStartBackup) {
            message_code = "backup.command_failed";
        } else if (request.kind == contracts::ServiceRequestKind::kStartRestore) {
            message_code = "restore.command_failed";
        } else if (request.kind == contracts::ServiceRequestKind::kMountRecoveryPoint ||
                   request.kind == contracts::ServiceRequestKind::kUnmountSession) {
            message_code = "mount.command_failed";
        }
        // Log domain detail (never secrets): response_detail only has message_code.
        write_log(runtime, ServiceLogLevel::kWarning, "service.command_failed_detail",
                  request_detail(request) + "; detail=" + detail);
        return base::Result<contracts::ServiceResponse>::success(failure(
            result.error().code, request.request_id, request.kind, std::move(message_code)));
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

void merge_job_progress(contracts::JobPage& page, WorkerSupervisor* supervisor) {
    if (supervisor == nullptr) {
        return;
    }
    // Prefer supervisor cache (live quantums + TaskResult snapshot on completion).
    // When cache is cold, leave progress null — do not invent synthetic 1/1 bytes.
    for (auto& item : page.items) {
        if (auto live = supervisor->last_progress(item.job_id)) {
            item.progress = std::move(*live);
        }
        if (item.progress && item.progress->message_code.empty()) {
            item.progress->message_code = "job.running";
        }
    }
}

[[nodiscard]] base::Result<contracts::ServiceResponse>
jobs_response(const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime,
              const base::CancellationToken cancellation) {
    if (!runtime.control_plane) {
        return capability_unavailable(request);
    }
    const auto& query = std::get<contracts::JobListRequest>(request.payload);
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "control_plane.ready";

    auto fitted = fetch_page_within_frame_budget<contracts::JobPage>(
        response, query.page.maximum_results, [&](const std::uint32_t maximum_results) {
            auto scaled = query;
            scaled.page.maximum_results = maximum_results;
            auto page = runtime.control_plane->list_jobs(scaled, cancellation);
            if (!page) {
                return page;
            }
            merge_job_progress(page.value(), runtime.worker_supervisor);
            return page;
        });
    if (!fitted) {
        return base::Result<contracts::ServiceResponse>::success(failure(
            fitted.error().code, request.request_id, request.kind, "control_plane.query_failed"));
    }
    response.payload = std::move(fitted).value();
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

[[nodiscard]] base::Result<contracts::ServiceResponse>
mount_list_response(const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime,
                    const base::CancellationToken cancellation) {
    if (!runtime.mount_supervisor) {
        return capability_unavailable(request);
    }
    const auto& query = std::get<contracts::MountSessionListRequest>(request.payload);
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "mount.list_ready";
    auto fitted = fetch_page_within_frame_budget<contracts::MountSessionPage>(
        response, query.page.maximum_results, [&](const std::uint32_t maximum_results) {
            auto scaled = query;
            scaled.page.maximum_results = maximum_results;
            return runtime.mount_supervisor->list(scaled, cancellation);
        });
    if (!fitted) {
        return base::Result<contracts::ServiceResponse>::success(
            failure(fitted.error().code, request.request_id, request.kind, "mount.list_failed"));
    }
    response.payload = std::move(fitted).value();
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

} // namespace

[[nodiscard]] base::Result<contracts::ServiceResponse> browse_file_sources_response(
    const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime,
    const ServiceSessionContext& session, const base::CancellationToken cancellation) {
    if (runtime.file_browse == nullptr) {
        return capability_unavailable(request);
    }
    if (session.browser_session.session_id.empty()) {
        return base::Result<contracts::ServiceResponse>::success(
            failure(base::ErrorCode::kInvalidArgument, request.request_id, request.kind,
                    "file_browse.session_required"));
    }
    const auto& query = std::get<contracts::BrowseFileSourcesRequest>(request.payload);
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "file_browse.ready";
    auto fitted = fetch_page_within_frame_budget<contracts::FileSourceNodePage>(
        response, query.page.maximum_results, [&](const std::uint32_t maximum_results) {
            auto scaled = query;
            scaled.page.maximum_results = maximum_results;
            return runtime.file_browse->browse(session.browser_session, scaled, cancellation);
        });
    if (!fitted) {
        return base::Result<contracts::ServiceResponse>::success(failure(
            fitted.error().code, request.request_id, request.kind,
            fitted.error().message.empty() ? "file_browse.failed" : fitted.error().message));
    }
    response.payload = std::move(fitted).value();
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

[[nodiscard]] base::Result<contracts::ServiceResponse> list_repository_directories_response(
    const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime,
    const ServiceSessionContext& session, const base::CancellationToken cancellation) {
    if (runtime.repository_location_browse == nullptr ||
        session.browser_session.session_id.empty()) {
        return capability_unavailable(request);
    }
    const auto& query = std::get<contracts::RepositoryDirectoryListRequest>(request.payload);
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "repository.directories_ready";
    auto page = runtime.repository_location_browse->list_directories(
        session.browser_session.session_id, query, cancellation);
    if (!page) {
        return base::Result<contracts::ServiceResponse>::success(
            failure(page.error().code, request.request_id, request.kind,
                    repository_failure_message_code(page.error())));
    }
    response.payload = std::move(page).value();
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

[[nodiscard]] base::Result<contracts::ServiceResponse>
list_recovery_point_entries_response(const contracts::ServiceRequest& request,
                                     const ServiceRuntimeInfo& runtime,
                                     const base::CancellationToken cancellation) {
    if (runtime.control_plane == nullptr || runtime.storage_factory == nullptr) {
        return capability_unavailable(request);
    }
    const auto& query = std::get<contracts::ListRecoveryPointEntriesRequest>(request.payload);
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "file_recover.ready";
    auto fitted = fetch_page_within_frame_budget<contracts::RecoveryPointEntryPage>(
        response, query.page.maximum_results, [&](const std::uint32_t maximum_results) {
            auto scaled = query;
            scaled.page.maximum_results = maximum_results;
            return list_recovery_point_entries(*runtime.control_plane, *runtime.storage_factory,
                                               scaled, cancellation);
        });
    if (!fitted) {
        const auto& error = fitted.error();
        const auto message =
            error.message.empty() ? std::string("file_recover.failed") : error.message;
        return base::Result<contracts::ServiceResponse>::success(
            failure(error.code, request.request_id, request.kind, message));
    }
    response.payload = std::move(fitted).value();
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

[[nodiscard]] base::Result<contracts::ServiceResponse> prepare_file_restore_response(
    const contracts::ServiceRequest& request, const ServiceRuntimeInfo& runtime,
    const ServiceSessionContext& session, const base::CancellationToken cancellation) {
    if (runtime.worker_jobs == nullptr) {
        return capability_unavailable(request);
    }
    auto result = runtime.worker_jobs->prepare_file_restore(
        std::get<contracts::PrepareFileRestoreRequest>(request.payload), session.browser_session,
        cancellation);
    if (!result) {
        const auto& error = result.error();
        const auto message =
            error.message.empty() ? std::string("file_restore.preflight_failed") : error.message;
        write_log(runtime, ServiceLogLevel::kWarning, "file_restore.preflight_failed",
                  error.message.empty() ? "prepare file restore failed" : error.message);
        return base::Result<contracts::ServiceResponse>::success(
            failure(error.code, request.request_id, request.kind, message));
    }
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "file_restore.preflight_ok";
    response.payload = std::move(result).value();
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

[[nodiscard]] std::uint64_t host_now_utc_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

[[nodiscard]] contracts::ServiceSettings
to_service_settings(const ports::ServiceSettingsRecord& record) {
    contracts::ServiceSettings settings;
    settings.job_retention_months = record.job_retention_months;
    settings.updated_utc_ms = record.updated_utc_ms;
    return settings;
}

[[nodiscard]] base::Result<std::uint64_t> retention_cutoff_utc_ms(const std::uint8_t months,
                                                                  const std::uint64_t now_utc_ms) {
    if (!contracts::is_valid_job_retention_months(months)) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kInvalidArgument, "job retention months is invalid"});
    }
    const auto window =
        static_cast<std::uint64_t>(months) * contracts::kMillisecondsPerRetentionMonth;
    if (now_utc_ms <= window) {
        return base::Result<std::uint64_t>::success(0);
    }
    return base::Result<std::uint64_t>::success(now_utc_ms - window);
}

[[nodiscard]] base::Result<std::uint64_t>
purge_expired_jobs(ports::IControlPlaneUnitOfWork& unit, const std::uint8_t retention_months,
                   const std::uint64_t now_utc_ms, const base::CancellationToken cancellation) {
    auto cutoff = retention_cutoff_utc_ms(retention_months, now_utc_ms);
    if (!cutoff) {
        return base::Result<std::uint64_t>::failure(cutoff.error());
    }
    return unit.jobs().purge_terminal_completed_before(cutoff.value(), cancellation);
}

[[nodiscard]] base::Result<contracts::ServiceResponse>
service_settings_response(const contracts::ServiceRequest& request,
                          const ServiceRuntimeInfo& runtime,
                          const base::CancellationToken cancellation) {
    if (runtime.control_plane == nullptr) {
        return capability_unavailable(request);
    }
    auto loaded = runtime.control_plane->get_service_settings(cancellation);
    if (!loaded) {
        return base::Result<contracts::ServiceResponse>::success(failure(
            loaded.error().code, request.request_id, request.kind, "service.settings_load_failed"));
    }
    auto settings = to_service_settings(loaded.value());
    auto valid = contracts::validate_service_settings(settings);
    if (!valid) {
        return base::Result<contracts::ServiceResponse>::failure(valid.error());
    }
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "service.settings_ready";
    response.payload = std::move(settings);
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

[[nodiscard]] base::Result<contracts::ServiceResponse>
update_service_settings_response(const contracts::ServiceRequest& request,
                                 const ServiceRuntimeInfo& runtime,
                                 const base::CancellationToken cancellation) {
    if (runtime.control_plane == nullptr || !request.idempotency_key) {
        return capability_unavailable(request);
    }
    const auto& command = std::get<contracts::UpdateServiceSettingsCommand>(request.payload);
    auto unit = runtime.control_plane->begin_unit_of_work(cancellation);
    if (!unit) {
        return base::Result<contracts::ServiceResponse>::success(failure(
            unit.error().code, request.request_id, request.kind, "service.settings_update_failed"));
    }
    const auto existing = unit.value()->commands().get(*request.idempotency_key, cancellation);
    if (!existing) {
        unit.value()->rollback();
        return base::Result<contracts::ServiceResponse>::success(
            failure(existing.error().code, request.request_id, request.kind,
                    "service.settings_update_failed"));
    }
    const auto fingerprint = std::string("settings|job_retention_months=") +
                             std::to_string(static_cast<unsigned>(command.job_retention_months));
    if (existing.value()) {
        unit.value()->rollback();
        if (existing.value()->request_fingerprint != fingerprint) {
            return base::Result<contracts::ServiceResponse>::success(
                failure(base::ErrorCode::kConflict, request.request_id, request.kind,
                        "service.idempotency_conflict"));
        }
        contracts::CommandAcknowledgement ack;
        ack.command_id = existing.value()->command_id;
        ack.disposition = contracts::CommandDisposition::kReplayed;
        ack.resource_id = existing.value()->resource_id;
        contracts::ServiceResponse response;
        response.request_id = request.request_id;
        response.kind = contracts::ServiceResponseKind::kCommandAccepted;
        response.request_kind = request.kind;
        response.boundary_error_code = base::ErrorCode::kNone;
        response.message_code = "service.settings_updated";
        response.payload = std::move(ack);
        return base::Result<contracts::ServiceResponse>::success(std::move(response));
    }
    const auto now_utc_ms = host_now_utc_ms();
    ports::ServiceSettingsRecord record;
    record.job_retention_months = command.job_retention_months;
    record.updated_utc_ms = now_utc_ms;
    auto written = unit.value()->service_settings().upsert(record, cancellation);
    if (!written) {
        unit.value()->rollback();
        return base::Result<contracts::ServiceResponse>::success(
            failure(written.error().code, request.request_id, request.kind,
                    "service.settings_update_failed"));
    }
    auto purged =
        purge_expired_jobs(*unit.value(), record.job_retention_months, now_utc_ms, cancellation);
    if (!purged) {
        unit.value()->rollback();
        return base::Result<contracts::ServiceResponse>::success(
            failure(purged.error().code, request.request_id, request.kind,
                    "service.settings_purge_failed"));
    }
    ports::CommandRecord command_record;
    command_record.idempotency_key = *request.idempotency_key;
    command_record.request_fingerprint = fingerprint;
    command_record.command_id = *request.idempotency_key;
    command_record.resource_id = "service.settings";
    command_record.created_utc_ms = now_utc_ms;
    auto inserted = unit.value()->commands().insert(command_record, cancellation);
    if (!inserted) {
        unit.value()->rollback();
        return base::Result<contracts::ServiceResponse>::success(
            failure(inserted.error().code, request.request_id, request.kind,
                    "service.settings_update_failed"));
    }
    auto committed = unit.value()->commit(cancellation);
    if (!committed) {
        return base::Result<contracts::ServiceResponse>::success(
            failure(committed.error().code, request.request_id, request.kind,
                    "service.settings_update_failed"));
    }
    contracts::CommandAcknowledgement ack;
    ack.command_id = command_record.command_id;
    ack.disposition = contracts::CommandDisposition::kAccepted;
    ack.resource_id = command_record.resource_id;
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kCommandAccepted;
    response.request_kind = request.kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "service.settings_updated";
    response.payload = std::move(ack);
    return base::Result<contracts::ServiceResponse>::success(std::move(response));
}

base::Result<contracts::ServiceResponse>
dispatch_service_request(const contracts::ServiceRequest& request,
                         const ServiceRuntimeInfo& runtime, const ServiceSessionContext& session,
                         const base::CancellationToken cancellation) {
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
    case contracts::ServiceRequestKind::kPrepareRestore:
    case contracts::ServiceRequestKind::kAnalyzeNtfsShrink:
        response = query_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kResolveRecoveryPointChain:
    case contracts::ServiceRequestKind::kPlanDeleteRecoveryPoints:
        response = recovery_point_ops_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kGetRecoveryPointLayout: {
        if (runtime.control_plane == nullptr || runtime.storage_factory == nullptr) {
            response = capability_unavailable(request);
            break;
        }
        auto layout = load_recovery_point_layout(
            *runtime.control_plane, *runtime.storage_factory,
            std::get<contracts::RecoveryPointRef>(request.payload), cancellation);
        if (!layout) {
            response = base::Result<contracts::ServiceResponse>::success(
                failure(layout.error().code, request.request_id, request.kind,
                        "recovery_point.layout_failed"));
            break;
        }
        contracts::ServiceResponse ok;
        ok.request_id = request.request_id;
        ok.kind = contracts::ServiceResponseKind::kQueryResult;
        ok.request_kind = request.kind;
        ok.boundary_error_code = base::ErrorCode::kNone;
        ok.message_code = "recovery_point.layout_ready";
        ok.payload = std::move(layout).value();
        response = base::Result<contracts::ServiceResponse>::success(std::move(ok));
        break;
    }
    case contracts::ServiceRequestKind::kListRepositoryDirectories:
        response = list_repository_directories_response(request, runtime, session, cancellation);
        break;
    case contracts::ServiceRequestKind::kAddRepositoryConnection:
    case contracts::ServiceRequestKind::kImportRepositoryConnection:
    case contracts::ServiceRequestKind::kTestRepositoryConnection:
    case contracts::ServiceRequestKind::kSetDefaultRepository:
    case contracts::ServiceRequestKind::kRemoveRepositoryConnection:
    case contracts::ServiceRequestKind::kConnectRepositoryLocation:
    case contracts::ServiceRequestKind::kStartBackup:
    case contracts::ServiceRequestKind::kStartVerify:
    case contracts::ServiceRequestKind::kStartRestore:
    case contracts::ServiceRequestKind::kStartFileRestore:
    case contracts::ServiceRequestKind::kExecuteDeletePlan:
    case contracts::ServiceRequestKind::kCancelJob:
        response = command_response(request, runtime, session, cancellation);
        break;
    case contracts::ServiceRequestKind::kListSchedules:
        response = query_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kUpsertSchedule:
    case contracts::ServiceRequestKind::kDeleteSchedule:
        response = command_response(request, runtime, session, cancellation);
        break;
    case contracts::ServiceRequestKind::kListMountSessions:
        response = mount_list_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kMountRecoveryPoint:
    case contracts::ServiceRequestKind::kUnmountSession:
        response = command_response(request, runtime, session, cancellation);
        break;
    case contracts::ServiceRequestKind::kListEvents:
    case contracts::ServiceRequestKind::kSubscribeTaskEvents:
    case contracts::ServiceRequestKind::kAcknowledgeEvents:
        response = capability_unavailable(request);
        break;
    case contracts::ServiceRequestKind::kBrowseFileSources:
        response = browse_file_sources_response(request, runtime, session, cancellation);
        break;
    case contracts::ServiceRequestKind::kListRecoveryPointEntries:
        response = list_recovery_point_entries_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kPrepareFileRestore:
        response = prepare_file_restore_response(request, runtime, session, cancellation);
        break;
    case contracts::ServiceRequestKind::kGetServiceSettings:
        response = service_settings_response(request, runtime, cancellation);
        break;
    case contracts::ServiceRequestKind::kUpdateServiceSettings:
        response = update_service_settings_response(request, runtime, cancellation);
        break;
    }
    if (!response) {
        write_log(runtime, ServiceLogLevel::kError, "service.dispatch_failed",
                  request_detail(request));
        return response;
    }
    if (response.value().kind == contracts::ServiceResponseKind::kRequestFailed) {
        write_log(runtime, ServiceLogLevel::kWarning, "service.request_failed",
                  response_detail(response.value()));
    }
    return response;
}

base::Result<std::string> handle_service_message(const std::string_view encoded_request,
                                                 const ServiceRuntimeInfo& runtime,
                                                 const ServiceSessionContext& session,
                                                 const base::CancellationToken cancellation) {
    write_interaction_log(runtime, "Inbound", encoded_request);
    auto request = decode_service_request(encoded_request);
    if (!request) {
        const auto code = request.error().code == base::ErrorCode::kUnsupportedVersion
                              ? base::ErrorCode::kUnsupportedVersion
                              : base::ErrorCode::kInvalidArgument;
        write_log(runtime, ServiceLogLevel::kWarning, "service.request_decode_failed",
                  "Request could not be decoded; error=" +
                      detail::readable_code(base::error_code_name(code)));
        auto encoded_response = encode_service_response(failure(code));
        if (encoded_response) {
            write_interaction_log(runtime, "Outbound", encoded_response.value());
        }
        return encoded_response;
    }
    auto response = dispatch_service_request(request.value(), runtime, session, cancellation);
    if (!response) {
        return base::Result<std::string>::failure(response.error());
    }
    auto encoded_response = encode_service_response(response.value());
    if (encoded_response) {
        write_interaction_log(runtime, "Outbound", encoded_response.value());
    }
    return encoded_response;
}

base::Result<void> run_service_session(ports::IMessageChannel& channel,
                                       const ServiceRuntimeInfo& runtime,
                                       const ServiceSessionContext& session,
                                       const base::CancellationToken& cancellation,
                                       const std::size_t maximum_requests) {
    auto result = detail::run_concurrent_service_session(channel, runtime, session, cancellation,
                                                         maximum_requests);
    if (runtime.file_browse != nullptr && !session.browser_session.session_id.empty()) {
        runtime.file_browse->clear_session(session.browser_session.session_id);
    }
    if (runtime.repository_location_browse != nullptr &&
        !session.browser_session.session_id.empty()) {
        runtime.repository_location_browse->clear_session(session.browser_session.session_id);
    }
    return result;
}

} // namespace aegra::apps::service
