#include "service_protocol_json.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace aegra::apps::service::protocol_json {
namespace {

[[nodiscard]] Json
encode_recovery_point_request(const contracts::RecoveryPointListRequest& request);
[[nodiscard]] contracts::RecoveryPointListRequest parse_recovery_point_request(const Json& payload);

[[nodiscard]] Json encode_page_request(const contracts::ServicePageRequest& request) {
    return Json{{"maximum_results", request.maximum_results},
                {"continuation_token", optional_string_json(request.continuation_token)}};
}

[[nodiscard]] contracts::ServicePageRequest parse_page_request(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"maximum_results", "continuation_token"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("service page request fields are invalid");
    }
    return {unsigned_value<std::uint32_t>(payload, "maximum_results"),
            optional_string(payload.at("continuation_token"))};
}

template <typename Enum> [[nodiscard]] Json optional_enum_json(const std::optional<Enum>& value) {
    return value ? Json(static_cast<std::uint8_t>(*value)) : Json(nullptr);
}

template <typename Enum> [[nodiscard]] std::optional<Enum> parse_optional_enum(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_number_unsigned()) {
        throw std::invalid_argument("service protocol optional enum is invalid");
    }
    const auto decoded = value.get<std::uint64_t>();
    if (decoded > (std::numeric_limits<std::uint8_t>::max)()) {
        throw std::out_of_range("service protocol optional enum is out of range");
    }
    return static_cast<Enum>(static_cast<std::uint8_t>(decoded));
}

[[nodiscard]] Json
encode_service_recovery_point_request(const contracts::ServiceRecoveryPointListRequest& request) {
    return Json{
        {"repository_connection_id", optional_string_json(request.repository_connection_id)},
        {"page", encode_recovery_point_request(request.page)}};
}

[[nodiscard]] contracts::ServiceRecoveryPointListRequest
parse_service_recovery_point_request(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"repository_connection_id", "page"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("service recovery point request fields are invalid");
    }
    return {optional_string(payload.at("repository_connection_id")),
            parse_recovery_point_request(payload.at("page"))};
}

[[nodiscard]] Json
encode_repository_list_request(const contracts::RepositoryConnectionListRequest& request) {
    return Json{{"page", encode_page_request(request.page)},
                {"state", optional_enum_json(request.state)}};
}

[[nodiscard]] contracts::RepositoryConnectionListRequest
parse_repository_list_request(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"page", "state"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("repository list request fields are invalid");
    }
    return {parse_page_request(payload.at("page")),
            parse_optional_enum<contracts::RepositoryConnectionState>(payload.at("state"))};
}

[[nodiscard]] Json
encode_source_list_request(const contracts::SourceInventoryListRequest& request) {
    return Json{{"page", encode_page_request(request.page)},
                {"include_unavailable", request.include_unavailable}};
}

[[nodiscard]] contracts::SourceInventoryListRequest parse_source_list_request(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"page", "include_unavailable"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("source list request fields are invalid");
    }
    return {parse_page_request(payload.at("page")), payload.at("include_unavailable").get<bool>()};
}

[[nodiscard]] Json encode_job_list_request(const contracts::JobListRequest& request) {
    return Json{{"page", encode_page_request(request.page)},
                {"operation", optional_enum_json(request.operation)},
                {"state", optional_enum_json(request.state)}};
}

[[nodiscard]] contracts::JobListRequest parse_job_list_request(const Json& payload) {
    constexpr std::array<std::string_view, 3> keys{"page", "operation", "state"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("job list request fields are invalid");
    }
    return {parse_page_request(payload.at("page")),
            parse_optional_enum<contracts::JobOperation>(payload.at("operation")),
            parse_optional_enum<contracts::ServiceJobState>(payload.at("state"))};
}

[[nodiscard]] Json encode_schedule_list_request(const contracts::ScheduleListRequest& request) {
    return Json{{"page", encode_page_request(request.page)},
                {"enabled", request.enabled ? Json(*request.enabled) : Json(nullptr)}};
}

[[nodiscard]] contracts::ScheduleListRequest parse_schedule_list_request(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"page", "enabled"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("schedule list request fields are invalid");
    }
    std::optional<bool> enabled;
    if (!payload.at("enabled").is_null()) {
        enabled = payload.at("enabled").get<bool>();
    }
    return {parse_page_request(payload.at("page")), enabled};
}

[[nodiscard]] Json
encode_audit_event_list_request(const contracts::AuditEventListRequest& request) {
    return Json{{"page", encode_page_request(request.page)},
                {"minimum_severity", optional_enum_json(request.minimum_severity)},
                {"from_utc_ms", optional_uint64_json(request.from_utc_ms)},
                {"to_utc_ms", optional_uint64_json(request.to_utc_ms)},
                {"correlation_id", optional_string_json(request.correlation_id)}};
}

[[nodiscard]] contracts::AuditEventListRequest parse_audit_event_list_request(const Json& payload) {
    constexpr std::array<std::string_view, 5> keys{"page", "minimum_severity", "from_utc_ms",
                                                   "to_utc_ms", "correlation_id"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("audit event list request fields are invalid");
    }
    return {parse_page_request(payload.at("page")),
            parse_optional_enum<contracts::AuditSeverity>(payload.at("minimum_severity")),
            optional_uint64(payload.at("from_utc_ms")), optional_uint64(payload.at("to_utc_ms")),
            optional_string(payload.at("correlation_id"))};
}

[[nodiscard]] Json encode_mount_list_request(const contracts::MountSessionListRequest& request) {
    return Json{{"page", encode_page_request(request.page)},
                {"state", optional_enum_json(request.state)}};
}

[[nodiscard]] contracts::MountSessionListRequest parse_mount_list_request(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"page", "state"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("mount list request fields are invalid");
    }
    return {parse_page_request(payload.at("page")),
            parse_optional_enum<contracts::MountSessionState>(payload.at("state"))};
}

[[nodiscard]] Json
encode_recovery_point_request(const contracts::RecoveryPointListRequest& request) {
    return Json{{"maximum_results", request.maximum_results},
                {"continuation_token", optional_string_json(request.continuation_token)}};
}

[[nodiscard]] contracts::RecoveryPointListRequest
parse_recovery_point_request(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"maximum_results", "continuation_token"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("recovery point request fields are invalid");
    }
    return {unsigned_value<std::uint32_t>(payload, "maximum_results"),
            optional_string(payload.at("continuation_token"))};
}

[[nodiscard]] Json encode_repository_input(const contracts::RepositoryConnectionInput& input) {
    return Json{{"display_name", input.display_name},
                {"locator", input.locator},
                {"credential_ref",
                 input.credential_ref ? Json(input.credential_ref->value) : Json(nullptr)}};
}

[[nodiscard]] contracts::RepositoryConnectionInput parse_repository_input(const Json& payload) {
    constexpr std::array<std::string_view, 3> keys{"display_name", "locator", "credential_ref"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("repository connection input fields are invalid");
    }
    contracts::RepositoryConnectionInput input;
    input.display_name = payload.at("display_name").get<std::string>();
    input.locator = payload.at("locator").get<std::string>();
    const auto credential = optional_string(payload.at("credential_ref"));
    if (credential) {
        input.credential_ref = contracts::SecretRef{*credential};
    }
    return input;
}

[[nodiscard]] Json encode_resource_ref(const contracts::ResourceRef& reference) {
    return Json{{"resource_id", reference.resource_id}};
}

[[nodiscard]] contracts::ResourceRef parse_resource_ref(const Json& payload) {
    constexpr std::array<std::string_view, 1> keys{"resource_id"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("resource reference fields are invalid");
    }
    return {payload.at("resource_id").get<std::string>()};
}

[[nodiscard]] Json encode_recovery_point_ref(const contracts::RecoveryPointRef& reference) {
    return Json{{"repository_connection_id", reference.repository_connection_id},
                {"recovery_point_id", reference.recovery_point_id}};
}

[[nodiscard]] contracts::RecoveryPointRef parse_recovery_point_ref(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"repository_connection_id", "recovery_point_id"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("recovery point reference fields are invalid");
    }
    return {payload.at("repository_connection_id").get<std::string>(),
            payload.at("recovery_point_id").get<std::string>()};
}

[[nodiscard]] Json encode_start_verify(const contracts::StartVerifyCommand& command) {
    return Json{{"repository_connection_id", command.repository_connection_id},
                {"recovery_point_id", command.recovery_point_id}};
}

[[nodiscard]] contracts::StartVerifyCommand parse_start_verify(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"repository_connection_id", "recovery_point_id"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("start verify fields are invalid");
    }
    return {payload.at("repository_connection_id").get<std::string>(),
            payload.at("recovery_point_id").get<std::string>()};
}

[[nodiscard]] Json encode_execute_delete_plan(const contracts::ExecuteDeletePlanCommand& command) {
    return Json{{"plan_token", command.plan_token}, {"confirmed", command.confirmed}};
}

[[nodiscard]] contracts::ExecuteDeletePlanCommand parse_execute_delete_plan(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"plan_token", "confirmed"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("execute delete plan fields are invalid");
    }
    return {payload.at("plan_token").get<std::string>(), payload.at("confirmed").get<bool>()};
}

[[nodiscard]] Json encode_start_backup(const contracts::StartBackupCommand& command) {
    return Json{
        {"source_id", command.source_id},
        {"repository_connection_id", command.repository_connection_id},
        {"backup_type", static_cast<std::uint8_t>(command.backup_type)},
        {"parent_recovery_point_id", optional_string_json(command.parent_recovery_point_id)}};
}

[[nodiscard]] contracts::StartBackupCommand parse_start_backup(const Json& payload) {
    constexpr std::array<std::string_view, 4> keys{"source_id", "repository_connection_id",
                                                   "backup_type", "parent_recovery_point_id"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("start backup fields are invalid");
    }
    contracts::StartBackupCommand command;
    command.source_id = payload.at("source_id").get<std::string>();
    command.repository_connection_id = payload.at("repository_connection_id").get<std::string>();
    command.backup_type =
        static_cast<contracts::BackupType>(unsigned_value<std::uint8_t>(payload, "backup_type"));
    command.parent_recovery_point_id = optional_string(payload.at("parent_recovery_point_id"));
    return command;
}

[[nodiscard]] Json
encode_restore_preflight_request(const contracts::RestorePreflightRequest& request) {
    return Json{{"repository_connection_id", request.repository_connection_id},
                {"recovery_point_id", request.recovery_point_id},
                {"target_source_id", request.target_source_id}};
}

[[nodiscard]] contracts::RestorePreflightRequest
parse_restore_preflight_request(const Json& payload) {
    constexpr std::array<std::string_view, 3> keys{"repository_connection_id", "recovery_point_id",
                                                   "target_source_id"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("restore preflight request fields are invalid");
    }
    return {payload.at("repository_connection_id").get<std::string>(),
            payload.at("recovery_point_id").get<std::string>(),
            payload.at("target_source_id").get<std::string>()};
}

[[nodiscard]] Json encode_start_restore(const contracts::StartRestoreCommand& command) {
    return Json{{"preflight_token", command.preflight_token}, {"confirmed", command.confirmed}};
}

[[nodiscard]] contracts::StartRestoreCommand parse_start_restore(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"preflight_token", "confirmed"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("start restore fields are invalid");
    }
    return {payload.at("preflight_token").get<std::string>(), payload.at("confirmed").get<bool>()};
}

[[nodiscard]] Json
encode_mount_recovery_point(const contracts::MountRecoveryPointCommand& command) {
    return Json{{"recovery_point_id", command.recovery_point_id},
                {"preferred_drive_letter", optional_string_json(command.preferred_drive_letter)}};
}

[[nodiscard]] contracts::MountRecoveryPointCommand parse_mount_recovery_point(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"recovery_point_id", "preferred_drive_letter"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("mount recovery point fields are invalid");
    }
    return {payload.at("recovery_point_id").get<std::string>(),
            optional_string(payload.at("preferred_drive_letter"))};
}

[[nodiscard]] Json encode_schedule_trigger(const contracts::ScheduleTrigger& trigger) {
    return Json{{"kind", static_cast<std::uint8_t>(trigger.kind)},
                {"local_minute_of_day", trigger.local_minute_of_day},
                {"weekday_mask", trigger.weekday_mask},
                {"timezone_id", trigger.timezone_id}};
}

[[nodiscard]] contracts::ScheduleTrigger parse_schedule_trigger(const Json& payload) {
    constexpr std::array<std::string_view, 4> keys{"kind", "local_minute_of_day", "weekday_mask",
                                                   "timezone_id"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("schedule trigger fields are invalid");
    }
    contracts::ScheduleTrigger trigger;
    trigger.kind =
        static_cast<contracts::ScheduleTriggerKind>(unsigned_value<std::uint8_t>(payload, "kind"));
    trigger.local_minute_of_day = unsigned_value<std::uint16_t>(payload, "local_minute_of_day");
    trigger.weekday_mask = unsigned_value<std::uint8_t>(payload, "weekday_mask");
    trigger.timezone_id = payload.at("timezone_id").get<std::string>();
    return trigger;
}

[[nodiscard]] Json encode_upsert_schedule(const contracts::UpsertScheduleCommand& command) {
    return Json{{"schedule_id", optional_string_json(command.schedule_id)},
                {"display_name", command.display_name},
                {"enabled", command.enabled},
                {"source_id", command.source_id},
                {"repository_connection_id", command.repository_connection_id},
                {"backup_type", static_cast<std::uint8_t>(command.backup_type)},
                {"trigger", encode_schedule_trigger(command.trigger)}};
}

[[nodiscard]] contracts::UpsertScheduleCommand parse_upsert_schedule(const Json& payload) {
    constexpr std::array<std::string_view, 7> keys{
        "schedule_id", "display_name", "enabled", "source_id", "repository_connection_id",
        "backup_type", "trigger"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("upsert schedule fields are invalid");
    }
    contracts::UpsertScheduleCommand command;
    command.schedule_id = optional_string(payload.at("schedule_id"));
    command.display_name = payload.at("display_name").get<std::string>();
    command.enabled = payload.at("enabled").get<bool>();
    command.source_id = payload.at("source_id").get<std::string>();
    command.repository_connection_id = payload.at("repository_connection_id").get<std::string>();
    command.backup_type =
        static_cast<contracts::BackupType>(unsigned_value<std::uint8_t>(payload, "backup_type"));
    command.trigger = parse_schedule_trigger(payload.at("trigger"));
    return command;
}

[[nodiscard]] Json encode_event_subscription(const contracts::EventSubscriptionRequest& request) {
    return Json{{"resume_token", optional_string_json(request.resume_token)},
                {"after_sequence", request.after_sequence},
                {"maximum_unacknowledged_events", request.maximum_unacknowledged_events}};
}

[[nodiscard]] contracts::EventSubscriptionRequest parse_event_subscription(const Json& payload) {
    constexpr std::array<std::string_view, 3> keys{"resume_token", "after_sequence",
                                                   "maximum_unacknowledged_events"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("event subscription fields are invalid");
    }
    return {optional_string(payload.at("resume_token")),
            unsigned_value<std::uint64_t>(payload, "after_sequence"),
            unsigned_value<std::uint32_t>(payload, "maximum_unacknowledged_events")};
}

[[nodiscard]] Json
encode_event_acknowledgement(const contracts::EventAcknowledgement& acknowledgement) {
    return Json{{"subscription_id", acknowledgement.subscription_id},
                {"through_sequence", acknowledgement.through_sequence}};
}

[[nodiscard]] contracts::EventAcknowledgement parse_event_acknowledgement(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"subscription_id", "through_sequence"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("event acknowledgement fields are invalid");
    }
    return {payload.at("subscription_id").get<std::string>(),
            unsigned_value<std::uint64_t>(payload, "through_sequence")};
}

} // namespace

std::optional<std::string> optional_string(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_string()) {
        throw std::invalid_argument("service protocol optional string is invalid");
    }
    return value.get<std::string>();
}

Json optional_string_json(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

std::optional<std::uint64_t> optional_uint64(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_number_unsigned()) {
        throw std::invalid_argument("service protocol optional integer is invalid");
    }
    return value.get<std::uint64_t>();
}

Json optional_uint64_json(const std::optional<std::uint64_t>& value) {
    return value ? Json(*value) : Json(nullptr);
}

Json encode_message_arguments(const contracts::MessageArguments& arguments) {
    Json encoded = Json::array();
    for (const auto& argument : arguments) {
        encoded.push_back(Json{{"name", argument.name}, {"value", argument.value}});
    }
    return encoded;
}

contracts::MessageArguments parse_message_arguments(const Json& value) {
    if (!value.is_array()) {
        throw std::invalid_argument("message arguments are invalid");
    }
    contracts::MessageArguments arguments;
    for (const auto& encoded : value) {
        constexpr std::array<std::string_view, 2> keys{"name", "value"};
        if (!exact_keys(encoded, keys)) {
            throw std::invalid_argument("message argument fields are invalid");
        }
        arguments.push_back(
            {encoded.at("name").get<std::string>(), encoded.at("value").get<std::string>()});
    }
    return arguments;
}

Json encode_request_payload(const contracts::ServiceRequest& request) {
    switch (request.kind) {
    case contracts::ServiceRequestKind::kGetServiceInfo: {
        const auto& range = std::get<contracts::ServiceVersionRange>(request.payload);
        return Json{{"minimum_api_version", range.minimum_api_version},
                    {"maximum_api_version", range.maximum_api_version}};
    }
    case contracts::ServiceRequestKind::kListRecoveryPoints:
        return encode_service_recovery_point_request(
            std::get<contracts::ServiceRecoveryPointListRequest>(request.payload));
    case contracts::ServiceRequestKind::kListRepositoryConnections:
        return encode_repository_list_request(
            std::get<contracts::RepositoryConnectionListRequest>(request.payload));
    case contracts::ServiceRequestKind::kListSourceInventory:
        return encode_source_list_request(
            std::get<contracts::SourceInventoryListRequest>(request.payload));
    case contracts::ServiceRequestKind::kListJobs:
        return encode_job_list_request(std::get<contracts::JobListRequest>(request.payload));
    case contracts::ServiceRequestKind::kListSchedules:
        return encode_schedule_list_request(
            std::get<contracts::ScheduleListRequest>(request.payload));
    case contracts::ServiceRequestKind::kListEvents:
        return encode_audit_event_list_request(
            std::get<contracts::AuditEventListRequest>(request.payload));
    case contracts::ServiceRequestKind::kListMountSessions:
        return encode_mount_list_request(
            std::get<contracts::MountSessionListRequest>(request.payload));
    case contracts::ServiceRequestKind::kPrepareRestore:
        return encode_restore_preflight_request(
            std::get<contracts::RestorePreflightRequest>(request.payload));
    case contracts::ServiceRequestKind::kResolveRecoveryPointChain:
    case contracts::ServiceRequestKind::kPlanDeleteRecoveryPoints:
        return encode_recovery_point_ref(std::get<contracts::RecoveryPointRef>(request.payload));
    case contracts::ServiceRequestKind::kAddRepositoryConnection:
    case contracts::ServiceRequestKind::kImportRepositoryConnection:
        return encode_repository_input(
            std::get<contracts::RepositoryConnectionInput>(request.payload));
    case contracts::ServiceRequestKind::kTestRepositoryConnection:
    case contracts::ServiceRequestKind::kSetDefaultRepository:
    case contracts::ServiceRequestKind::kRemoveRepositoryConnection:
    case contracts::ServiceRequestKind::kCancelJob:
    case contracts::ServiceRequestKind::kUnmountSession:
    case contracts::ServiceRequestKind::kDeleteSchedule:
        return encode_resource_ref(std::get<contracts::ResourceRef>(request.payload));
    case contracts::ServiceRequestKind::kStartBackup:
        return encode_start_backup(std::get<contracts::StartBackupCommand>(request.payload));
    case contracts::ServiceRequestKind::kStartVerify:
        return encode_start_verify(std::get<contracts::StartVerifyCommand>(request.payload));
    case contracts::ServiceRequestKind::kStartRestore:
        return encode_start_restore(std::get<contracts::StartRestoreCommand>(request.payload));
    case contracts::ServiceRequestKind::kMountRecoveryPoint:
        return encode_mount_recovery_point(
            std::get<contracts::MountRecoveryPointCommand>(request.payload));
    case contracts::ServiceRequestKind::kUpsertSchedule:
        return encode_upsert_schedule(std::get<contracts::UpsertScheduleCommand>(request.payload));
    case contracts::ServiceRequestKind::kSubscribeTaskEvents:
        return encode_event_subscription(
            std::get<contracts::EventSubscriptionRequest>(request.payload));
    case contracts::ServiceRequestKind::kAcknowledgeEvents:
        return encode_event_acknowledgement(
            std::get<contracts::EventAcknowledgement>(request.payload));
    case contracts::ServiceRequestKind::kExecuteDeletePlan:
        return encode_execute_delete_plan(
            std::get<contracts::ExecuteDeletePlanCommand>(request.payload));
    }
    throw std::invalid_argument("service request kind is invalid");
}

contracts::ServiceRequestPayload parse_request_payload(const contracts::ServiceRequestKind kind,
                                                       const Json& payload) {
    switch (kind) {
    case contracts::ServiceRequestKind::kGetServiceInfo: {
        constexpr std::array<std::string_view, 2> keys{"minimum_api_version",
                                                       "maximum_api_version"};
        if (!exact_keys(payload, keys)) {
            throw std::invalid_argument("service version range fields are invalid");
        }
        return contracts::ServiceVersionRange{
            unsigned_value<std::uint32_t>(payload, "minimum_api_version"),
            unsigned_value<std::uint32_t>(payload, "maximum_api_version")};
    }
    case contracts::ServiceRequestKind::kListRecoveryPoints:
        return parse_service_recovery_point_request(payload);
    case contracts::ServiceRequestKind::kListRepositoryConnections:
        return parse_repository_list_request(payload);
    case contracts::ServiceRequestKind::kListSourceInventory:
        return parse_source_list_request(payload);
    case contracts::ServiceRequestKind::kListJobs:
        return parse_job_list_request(payload);
    case contracts::ServiceRequestKind::kListSchedules:
        return parse_schedule_list_request(payload);
    case contracts::ServiceRequestKind::kListEvents:
        return parse_audit_event_list_request(payload);
    case contracts::ServiceRequestKind::kListMountSessions:
        return parse_mount_list_request(payload);
    case contracts::ServiceRequestKind::kPrepareRestore:
        return parse_restore_preflight_request(payload);
    case contracts::ServiceRequestKind::kResolveRecoveryPointChain:
    case contracts::ServiceRequestKind::kPlanDeleteRecoveryPoints:
        return parse_recovery_point_ref(payload);
    case contracts::ServiceRequestKind::kAddRepositoryConnection:
    case contracts::ServiceRequestKind::kImportRepositoryConnection:
        return parse_repository_input(payload);
    case contracts::ServiceRequestKind::kTestRepositoryConnection:
    case contracts::ServiceRequestKind::kSetDefaultRepository:
    case contracts::ServiceRequestKind::kRemoveRepositoryConnection:
    case contracts::ServiceRequestKind::kCancelJob:
    case contracts::ServiceRequestKind::kUnmountSession:
    case contracts::ServiceRequestKind::kDeleteSchedule:
        return parse_resource_ref(payload);
    case contracts::ServiceRequestKind::kStartBackup:
        return parse_start_backup(payload);
    case contracts::ServiceRequestKind::kStartVerify:
        return parse_start_verify(payload);
    case contracts::ServiceRequestKind::kStartRestore:
        return parse_start_restore(payload);
    case contracts::ServiceRequestKind::kMountRecoveryPoint:
        return parse_mount_recovery_point(payload);
    case contracts::ServiceRequestKind::kUpsertSchedule:
        return parse_upsert_schedule(payload);
    case contracts::ServiceRequestKind::kSubscribeTaskEvents:
        return parse_event_subscription(payload);
    case contracts::ServiceRequestKind::kAcknowledgeEvents:
        return parse_event_acknowledgement(payload);
    case contracts::ServiceRequestKind::kExecuteDeletePlan:
        return parse_execute_delete_plan(payload);
    }
    throw std::invalid_argument("service request kind is invalid");
}

} // namespace aegra::apps::service::protocol_json
