#include "aegra/contracts/service.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>
#include <type_traits>
#include <variant>

namespace aegra::contracts {
namespace {

constexpr std::size_t kMaximumRequestIdBytes = 128;
constexpr std::size_t kMaximumVersionBytes = 64;
constexpr std::size_t kMaximumMessageCodeBytes = 128;
constexpr std::size_t kMaximumCapabilities = 64;
constexpr std::size_t kMaximumCapabilityBytes = 64;
constexpr std::size_t kMaximumIdempotencyKeyBytes = 128;

[[nodiscard]] base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure({base::ErrorCode::kInvalidArgument, message});
}

[[nodiscard]] bool valid_code_character(const unsigned char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.' ||
           value == '_' || value == '-' || value == ':';
}

[[nodiscard]] bool valid_stable_code(const std::string_view value,
                                     const std::size_t maximum_bytes) noexcept {
    return !value.empty() && value.size() <= maximum_bytes &&
           std::ranges::all_of(value, valid_code_character);
}

[[nodiscard]] bool valid_request_id(const std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumRequestIdBytes) {
        return false;
    }
    return std::ranges::all_of(value, [](const unsigned char character) {
        return character >= 0x21U && character <= 0x7EU;
    });
}

[[nodiscard]] bool known_request_kind(const ServiceRequestKind kind) noexcept {
    return is_service_query_kind(kind) || is_service_command_kind(kind);
}

[[nodiscard]] bool known_error_code(const base::ErrorCode code) noexcept {
    return code >= base::ErrorCode::kInvalidArgument && code <= base::ErrorCode::kOutcomeUnknown;
}

template <typename Payload, typename Validator>
[[nodiscard]] base::Result<void> validate_payload(const ServiceRequest& request,
                                                  Validator validator) {
    const auto* payload = std::get_if<Payload>(&request.payload);
    return payload != nullptr ? validator(*payload) : invalid("service request payload mismatch");
}

[[nodiscard]] base::Result<void> validate_resource_payload(const ServiceRequest& request) {
    return validate_payload<ResourceRef>(request, validate_resource_ref);
}

[[nodiscard]] base::Result<void> validate_query_payload(const ServiceRequest& request) {
    switch (request.kind) {
    case ServiceRequestKind::kGetServiceInfo:
        return validate_payload<ServiceVersionRange>(request, validate_service_version_range);
    case ServiceRequestKind::kListRecoveryPoints:
        return validate_payload<ServiceRecoveryPointListRequest>(
            request, validate_service_recovery_point_list_request);
    case ServiceRequestKind::kListRepositoryConnections:
        return validate_payload<RepositoryConnectionListRequest>(
            request, validate_repository_connection_list_request);
    case ServiceRequestKind::kListSourceInventory:
        return validate_payload<SourceInventoryListRequest>(request,
                                                            validate_source_inventory_list_request);
    case ServiceRequestKind::kListJobs:
        return validate_payload<JobListRequest>(request, validate_job_list_request);
    case ServiceRequestKind::kListSchedules:
        return validate_payload<ScheduleListRequest>(request, validate_schedule_list_request);
    case ServiceRequestKind::kListEvents:
        return validate_payload<AuditEventListRequest>(request, validate_audit_event_list_request);
    case ServiceRequestKind::kListMountSessions:
        return validate_payload<MountSessionListRequest>(request,
                                                         validate_mount_session_list_request);
    case ServiceRequestKind::kPrepareRestore:
        return validate_payload<RestorePreflightRequest>(request,
                                                         validate_restore_preflight_request);
    case ServiceRequestKind::kResolveRecoveryPointChain:
    case ServiceRequestKind::kPlanDeleteRecoveryPoints:
    case ServiceRequestKind::kGetRecoveryPointLayout:
        return validate_payload<RecoveryPointRef>(request, validate_recovery_point_ref);
    case ServiceRequestKind::kBrowseFileSources:
        return validate_payload<BrowseFileSourcesRequest>(request,
                                                          validate_browse_file_sources_request);
    case ServiceRequestKind::kListRecoveryPointEntries:
        return validate_payload<ListRecoveryPointEntriesRequest>(
            request, validate_list_recovery_point_entries_request);
    case ServiceRequestKind::kPrepareFileRestore:
        return validate_payload<PrepareFileRestoreRequest>(request,
                                                           validate_prepare_file_restore_request);
    case ServiceRequestKind::kGetServiceSettings:
        return validate_payload<ServiceSettingsQuery>(request, validate_service_settings_query);
    case ServiceRequestKind::kListRepositoryDirectories:
        return validate_payload<RepositoryDirectoryListRequest>(
            request, validate_repository_directory_list_request);
    case ServiceRequestKind::kAnalyzeNtfsShrink:
        return validate_payload<RestorePreflightRequest>(request,
                                                         validate_restore_preflight_request);
    default:
        return invalid("service query kind is invalid");
    }
}

[[nodiscard]] base::Result<void> validate_command_payload(const ServiceRequest& request) {
    switch (request.kind) {
    case ServiceRequestKind::kAddRepositoryConnection:
    case ServiceRequestKind::kImportRepositoryConnection:
    case ServiceRequestKind::kConnectRepositoryLocation:
        return validate_payload<RepositoryConnectionInput>(request,
                                                           validate_repository_connection_input);
    case ServiceRequestKind::kTestRepositoryConnection:
    case ServiceRequestKind::kSetDefaultRepository:
    case ServiceRequestKind::kRemoveRepositoryConnection:
    case ServiceRequestKind::kCancelJob:
    case ServiceRequestKind::kUnmountSession:
    case ServiceRequestKind::kDeleteSchedule:
        return validate_resource_payload(request);
    case ServiceRequestKind::kStartBackup:
        return validate_payload<StartBackupCommand>(request, validate_start_backup_command);
    case ServiceRequestKind::kStartVerify:
        return validate_payload<StartVerifyCommand>(request, validate_start_verify_command);
    case ServiceRequestKind::kStartRestore:
        return validate_payload<StartRestoreCommand>(request, validate_start_restore_command);
    case ServiceRequestKind::kMountRecoveryPoint:
        return validate_payload<MountRecoveryPointCommand>(request,
                                                           validate_mount_recovery_point_command);
    case ServiceRequestKind::kUpsertSchedule:
        return validate_payload<UpsertScheduleCommand>(request, validate_upsert_schedule_command);
    case ServiceRequestKind::kSubscribeTaskEvents:
        return validate_payload<EventSubscriptionRequest>(request,
                                                          validate_event_subscription_request);
    case ServiceRequestKind::kAcknowledgeEvents:
        return validate_payload<EventAcknowledgement>(request, validate_event_acknowledgement);
    case ServiceRequestKind::kExecuteDeletePlan:
        return validate_payload<ExecuteDeletePlanCommand>(request,
                                                          validate_execute_delete_plan_command);
    case ServiceRequestKind::kStartFileRestore:
        return validate_payload<StartFileRestoreCommand>(request,
                                                         validate_start_file_restore_command);
    case ServiceRequestKind::kUpdateServiceSettings:
        return validate_payload<UpdateServiceSettingsCommand>(
            request, validate_update_service_settings_command);
    default:
        return invalid("service command kind is invalid");
    }
}

template <typename Payload, typename Validator>
[[nodiscard]] base::Result<void> validate_response_payload(const ServiceResponse& response,
                                                           Validator validator) {
    const auto* payload = std::get_if<Payload>(&response.payload);
    return payload != nullptr ? validator(*payload) : invalid("service response payload mismatch");
}

[[nodiscard]] base::Result<void> validate_query_response(const ServiceResponse& response) {
    switch (response.request_kind) {
    case ServiceRequestKind::kGetServiceInfo:
        return validate_response_payload<ServiceInfo>(response, validate_service_info);
    case ServiceRequestKind::kListRecoveryPoints:
        return validate_response_payload<ServiceRecoveryPointPage>(
            response, validate_service_recovery_point_page);
    case ServiceRequestKind::kListRepositoryConnections:
        return validate_response_payload<RepositoryConnectionPage>(
            response, validate_repository_connection_page);
    case ServiceRequestKind::kListSourceInventory:
        return validate_response_payload<SourceInventoryPage>(response,
                                                              validate_source_inventory_page);
    case ServiceRequestKind::kListJobs:
        return validate_response_payload<JobPage>(response, validate_job_page);
    case ServiceRequestKind::kListSchedules:
        return validate_response_payload<SchedulePage>(response, validate_schedule_page);
    case ServiceRequestKind::kListEvents:
        return validate_response_payload<AuditEventPage>(response, validate_audit_event_page);
    case ServiceRequestKind::kListMountSessions:
        return validate_response_payload<MountSessionPage>(response, validate_mount_session_page);
    case ServiceRequestKind::kPrepareRestore:
        return validate_response_payload<RestorePreflight>(response, validate_restore_preflight);
    case ServiceRequestKind::kResolveRecoveryPointChain:
        return validate_response_payload<RecoveryPointChainResult>(
            response, validate_recovery_point_chain_result);
    case ServiceRequestKind::kPlanDeleteRecoveryPoints:
        return validate_response_payload<DeletePlanSummary>(response, validate_delete_plan_summary);
    case ServiceRequestKind::kGetRecoveryPointLayout:
        return validate_response_payload<RecoveryPointLayout>(response,
                                                              validate_recovery_point_layout);
    case ServiceRequestKind::kBrowseFileSources:
        return validate_response_payload<FileSourceNodePage>(response,
                                                             validate_file_source_node_page);
    case ServiceRequestKind::kListRecoveryPointEntries:
        return validate_response_payload<RecoveryPointEntryPage>(
            response, validate_recovery_point_entry_page);
    case ServiceRequestKind::kPrepareFileRestore:
        return validate_response_payload<FileRestorePreflight>(response,
                                                               validate_file_restore_preflight);
    case ServiceRequestKind::kGetServiceSettings:
        return validate_response_payload<ServiceSettings>(response, validate_service_settings);
    case ServiceRequestKind::kListRepositoryDirectories:
        return validate_response_payload<FileSourceNodePage>(response,
                                                             validate_file_source_node_page);
    case ServiceRequestKind::kAnalyzeNtfsShrink:
        return validate_response_payload<RestorePreflight>(response, validate_restore_preflight);
    default:
        return invalid("service query response kind is invalid");
    }
}

[[nodiscard]] base::Result<void> validate_event_payload(const ServiceEvent& event) {
    switch (event.kind) {
    case ServiceEventKind::kTaskProgress: {
        const auto* payload = std::get_if<TaskProgress>(&event.payload);
        return payload != nullptr ? validate_task_progress(*payload)
                                  : invalid("task progress event payload mismatch");
    }
    case ServiceEventKind::kTaskCompleted: {
        const auto* payload = std::get_if<TaskResult>(&event.payload);
        return payload != nullptr ? validate_task_result(*payload)
                                  : invalid("task result event payload mismatch");
    }
    case ServiceEventKind::kMountSessionChanged: {
        const auto* payload = std::get_if<MountSessionSummary>(&event.payload);
        return payload != nullptr ? validate_mount_session_summary(*payload)
                                  : invalid("mount event payload mismatch");
    }
    }
    return invalid("service event kind is invalid");
}

} // namespace

bool is_service_query_kind(const ServiceRequestKind kind) noexcept {
    return kind >= ServiceRequestKind::kGetServiceInfo &&
           kind <= ServiceRequestKind::kAnalyzeNtfsShrink;
}

bool is_service_command_kind(const ServiceRequestKind kind) noexcept {
    return (kind >= ServiceRequestKind::kAddRepositoryConnection &&
            kind <= ServiceRequestKind::kExecuteDeletePlan) ||
           kind == ServiceRequestKind::kStartFileRestore ||
           kind == ServiceRequestKind::kUpdateServiceSettings ||
           kind == ServiceRequestKind::kConnectRepositoryLocation;
}

base::Result<void> validate_service_request(const ServiceRequest& request) {
    if (request.schema_version != kServiceRequestSchemaVersion) {
        return base::Result<void>::failure(
            {base::ErrorCode::kUnsupportedVersion, "unsupported service request schema version"});
    }
    if (request.message_type != ServiceMessageType::kRequest ||
        !valid_request_id(request.request_id) || !known_request_kind(request.kind)) {
        return invalid("service request envelope is invalid");
    }
    if (is_service_query_kind(request.kind)) {
        return request.idempotency_key ? invalid("service query cannot carry idempotency key")
                                       : validate_query_payload(request);
    }
    if (!request.idempotency_key ||
        !valid_stable_code(*request.idempotency_key, kMaximumIdempotencyKeyBytes)) {
        return invalid("service command idempotency key is invalid");
    }
    return validate_command_payload(request);
}

base::Result<void> validate_service_info(const ServiceInfo& service) {
    const auto known_state =
        service.state >= ServiceState::kStarting && service.state <= ServiceState::kStopping;
    if (service.minimum_api_version == 0 || service.minimum_api_version > service.api_version ||
        service.api_version != kServiceApiVersion || !known_state ||
        service.service_version.empty() || service.service_version.size() > kMaximumVersionBytes ||
        service.capabilities.empty() || service.capabilities.size() > kMaximumCapabilities) {
        return invalid("service info fields are invalid");
    }
    std::string_view previous;
    for (const auto& capability : service.capabilities) {
        if (!valid_stable_code(capability, kMaximumCapabilityBytes) ||
            (!previous.empty() && capability <= previous)) {
            return invalid("service capabilities are invalid or unsorted");
        }
        previous = capability;
    }
    return base::Result<void>::success();
}

base::Result<void> validate_service_response(const ServiceResponse& response) {
    if (response.schema_version != kServiceResponseSchemaVersion) {
        return base::Result<void>::failure(
            {base::ErrorCode::kUnsupportedVersion, "unsupported service response schema version"});
    }
    auto valid_arguments = validate_message_arguments(response.message_arguments);
    if (response.message_type != ServiceMessageType::kResponse ||
        !valid_stable_code(response.message_code, kMaximumMessageCodeBytes) || !valid_arguments ||
        !known_request_kind(response.request_kind)) {
        return invalid("service response envelope is invalid");
    }
    if (response.kind == ServiceResponseKind::kRequestFailed) {
        if ((!response.request_id.empty() && !valid_request_id(response.request_id)) ||
            !known_error_code(response.boundary_error_code) ||
            !std::holds_alternative<std::monostate>(response.payload)) {
            return invalid("service failure response is invalid");
        }
        return base::Result<void>::success();
    }
    if (!valid_request_id(response.request_id) ||
        response.boundary_error_code != base::ErrorCode::kNone) {
        return invalid("successful service response envelope is invalid");
    }
    if (response.kind == ServiceResponseKind::kQueryResult) {
        return is_service_query_kind(response.request_kind)
                   ? validate_query_response(response)
                   : invalid("query response references a command");
    }
    if (response.kind == ServiceResponseKind::kCommandAccepted) {
        if (!is_service_command_kind(response.request_kind)) {
            return invalid("command response references a query");
        }
        const auto* acknowledgement = std::get_if<CommandAcknowledgement>(&response.payload);
        if (acknowledgement == nullptr) {
            return invalid("service response payload mismatch");
        }
        const auto subscription_request =
            response.request_kind == ServiceRequestKind::kSubscribeTaskEvents;
        if (subscription_request != acknowledgement->event_subscription.has_value()) {
            return invalid("command subscription acknowledgement is invalid");
        }
        return validate_command_acknowledgement(*acknowledgement);
    }
    return invalid("service response kind is invalid");
}

base::Result<void> validate_service_event(const ServiceEvent& event) {
    auto valid_arguments = validate_message_arguments(event.message_arguments);
    if (event.schema_version != kServiceEventSchemaVersion ||
        event.message_type != ServiceMessageType::kEvent ||
        !valid_stable_code(event.subscription_id, kMaximumRequestIdBytes) || event.sequence == 0 ||
        event.sequence > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
        !valid_stable_code(event.message_code, kMaximumMessageCodeBytes) || !valid_arguments) {
        return invalid("service event envelope is invalid");
    }
    auto valid_payload = validate_event_payload(event);
    if (!valid_payload) {
        return valid_payload;
    }
    constexpr auto maximum_wire_integer =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if (const auto* progress = std::get_if<TaskProgress>(&event.payload)) {
        if ((progress->logical_bytes && *progress->logical_bytes > maximum_wire_integer) ||
            progress->processed_bytes > maximum_wire_integer ||
            progress->stored_bytes > maximum_wire_integer ||
            progress->discovered_entries > maximum_wire_integer ||
            progress->processed_entries > maximum_wire_integer ||
            !valid_stable_code(progress->message_code, kMaximumMessageCodeBytes)) {
            return invalid("service task progress exceeds the wire integer range");
        }
    }
    if (const auto* result = std::get_if<TaskResult>(&event.payload)) {
        if (result->logical_bytes > maximum_wire_integer ||
            result->stored_bytes > maximum_wire_integer ||
            result->chunk_count > maximum_wire_integer ||
            !valid_stable_code(result->message_code, kMaximumMessageCodeBytes) ||
            std::ranges::any_of(result->warning_codes, [](const std::string& code) {
                return !valid_stable_code(code, kMaximumMessageCodeBytes);
            })) {
            return invalid("service task result exceeds the wire integer range");
        }
    }
    return base::Result<void>::success();
}

} // namespace aegra::contracts
