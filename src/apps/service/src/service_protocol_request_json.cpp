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
                {"recovery_point_id", reference.recovery_point_id},
                {"archive_password", reference.archive_password}};
}

[[nodiscard]] contracts::RecoveryPointRef parse_recovery_point_ref(const Json& payload) {
    constexpr std::array<std::string_view, 3> keys{"repository_connection_id", "recovery_point_id",
                                                   "archive_password"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("recovery point reference fields are invalid");
    }
    return {payload.at("repository_connection_id").get<std::string>(),
            payload.at("recovery_point_id").get<std::string>(),
            payload.at("archive_password").get<std::string>()};
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
    return Json{{"schedule_id", command.schedule_id},
                {"backup_type", static_cast<std::uint8_t>(command.backup_type)}};
}

[[nodiscard]] contracts::StartBackupCommand parse_start_backup(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"schedule_id", "backup_type"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("start backup fields are invalid");
    }
    contracts::StartBackupCommand command;
    command.schedule_id = payload.at("schedule_id").get<std::string>();
    command.backup_type =
        static_cast<contracts::BackupType>(unsigned_value<std::uint8_t>(payload, "backup_type"));
    return command;
}

[[nodiscard]] Json
encode_restore_preflight_request(const contracts::RestorePreflightRequest& request) {
    return Json{{"repository_connection_id", request.repository_connection_id},
                {"recovery_point_id", request.recovery_point_id},
                {"target_source_id", request.target_source_id},
                {"source_disk_number", request.source_disk_number},
                {"source_volume_index", request.source_volume_index},
                {"archive_password", request.archive_password}};
}

[[nodiscard]] contracts::RestorePreflightRequest
parse_restore_preflight_request(const Json& payload) {
    constexpr std::array<std::string_view, 6> keys{
        "repository_connection_id", "recovery_point_id", "target_source_id", "source_disk_number",
        "source_volume_index", "archive_password"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("restore preflight request fields are invalid");
    }
    contracts::RestorePreflightRequest request;
    request.repository_connection_id = payload.at("repository_connection_id").get<std::string>();
    request.recovery_point_id = payload.at("recovery_point_id").get<std::string>();
    request.target_source_id = payload.at("target_source_id").get<std::string>();
    request.source_disk_number =
        unsigned_value<std::uint32_t>(payload, "source_disk_number");
    request.source_volume_index =
        unsigned_value<std::uint32_t>(payload, "source_volume_index");
    request.archive_password = payload.at("archive_password").get<std::string>();
    return request;
}

[[nodiscard]] Json encode_start_restore(const contracts::StartRestoreCommand& command) {
    return Json{{"preflight_token", command.preflight_token},
                {"confirmed", command.confirmed},
                {"archive_password", command.archive_password},
                {"preserve_disk_signature", command.preserve_disk_signature},
                {"auto_expand_last_partition", command.auto_expand_last_partition}};
}

[[nodiscard]] contracts::StartRestoreCommand parse_start_restore(const Json& payload) {
    constexpr std::array<std::string_view, 5> keys{
        "preflight_token", "confirmed", "archive_password", "preserve_disk_signature",
        "auto_expand_last_partition"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("start restore fields are invalid");
    }
    return {payload.at("preflight_token").get<std::string>(), payload.at("confirmed").get<bool>(),
            payload.at("archive_password").get<std::string>(),
            payload.at("preserve_disk_signature").get<bool>(),
            payload.at("auto_expand_last_partition").get<bool>()};
}

[[nodiscard]] Json
encode_mount_recovery_point(const contracts::MountRecoveryPointCommand& command) {
    return Json{{"repository_connection_id", command.repository_connection_id},
                {"recovery_point_id", command.recovery_point_id},
                {"source_disk_number", command.source_disk_number},
                {"preferred_drive_letter", optional_string_json(command.preferred_drive_letter)},
                {"archive_password", command.archive_password}};
}

[[nodiscard]] contracts::MountRecoveryPointCommand parse_mount_recovery_point(const Json& payload) {
    constexpr std::array<std::string_view, 5> keys{
        "repository_connection_id", "recovery_point_id", "source_disk_number",
        "preferred_drive_letter",   "archive_password"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("mount recovery point fields are invalid");
    }
    contracts::MountRecoveryPointCommand command;
    command.repository_connection_id = payload.at("repository_connection_id").get<std::string>();
    command.recovery_point_id = payload.at("recovery_point_id").get<std::string>();
    command.source_disk_number = unsigned_value<std::uint32_t>(payload, "source_disk_number");
    command.preferred_drive_letter = optional_string(payload.at("preferred_drive_letter"));
    command.archive_password = payload.at("archive_password").get<std::string>();
    return command;
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

[[nodiscard]] Json encode_protection_spec(const contracts::ProtectionSpecInput& protection) {
    Json volume = Json(nullptr);
    Json file = Json(nullptr);
    if (protection.content_kind == contracts::ContentKind::kVolumeSet) {
        volume = Json{{"source_ids", protection.volume_source_ids}};
    } else {
        Json selections = Json::array();
        for (const auto& item : protection.file_selections) {
            selections.push_back(
                Json{{"node_token", item.node_token},
                     {"recursion", static_cast<std::uint8_t>(item.recursion)},
                     {"display_label", item.display_label}});
        }
        file = Json{{"selections", std::move(selections)},
                    {"options",
                     Json{{"unreadable_policy",
                           static_cast<std::uint8_t>(
                               protection.file_options.unreadable_policy)}}}};
    }
    return Json{{"content_kind", static_cast<std::uint8_t>(protection.content_kind)},
                {"volume_set", std::move(volume)},
                {"file_set", std::move(file)}};
}

[[nodiscard]] contracts::ProtectionSpecInput parse_protection_spec(const Json& payload) {
    constexpr std::array<std::string_view, 3> keys{"content_kind", "volume_set", "file_set"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("protection spec fields are invalid");
    }
    contracts::ProtectionSpecInput protection;
    protection.content_kind =
        static_cast<contracts::ContentKind>(unsigned_value<std::uint8_t>(payload, "content_kind"));
    if (protection.content_kind == contracts::ContentKind::kVolumeSet) {
        if (payload.at("volume_set").is_null() || !payload.at("file_set").is_null()) {
            throw std::invalid_argument("volume protection requires volume_set only");
        }
        constexpr std::array<std::string_view, 1> volume_keys{"source_ids"};
        if (!exact_keys(payload.at("volume_set"), volume_keys)) {
            throw std::invalid_argument("volume protection fields are invalid");
        }
        protection.volume_source_ids =
            payload.at("volume_set").at("source_ids").get<std::vector<std::string>>();
        return protection;
    }
    if (payload.at("file_set").is_null() || !payload.at("volume_set").is_null()) {
        throw std::invalid_argument("file protection requires file_set only");
    }
    constexpr std::array<std::string_view, 2> file_keys{"selections", "options"};
    if (!exact_keys(payload.at("file_set"), file_keys)) {
        throw std::invalid_argument("file protection fields are invalid");
    }
    const auto& options = payload.at("file_set").at("options");
    constexpr std::array<std::string_view, 1> option_keys{"unreadable_policy"};
    if (!exact_keys(options, option_keys)) {
        throw std::invalid_argument("file protection options are invalid");
    }
    protection.file_options.unreadable_policy = static_cast<contracts::FileUnreadablePolicy>(
        unsigned_value<std::uint8_t>(options, "unreadable_policy"));
    for (const auto& item : payload.at("file_set").at("selections")) {
        constexpr std::array<std::string_view, 3> selection_keys{"node_token", "recursion",
                                                                 "display_label"};
        if (!exact_keys(item, selection_keys)) {
            throw std::invalid_argument("file selection fields are invalid");
        }
        contracts::FileSelectionInput selection;
        selection.node_token = item.at("node_token").get<std::string>();
        selection.recursion =
            static_cast<contracts::FileRecursion>(unsigned_value<std::uint8_t>(item, "recursion"));
        selection.display_label = item.at("display_label").get<std::string>();
        protection.file_selections.push_back(std::move(selection));
    }
    return protection;
}

[[nodiscard]] Json encode_upsert_schedule(const contracts::UpsertScheduleCommand& command) {
    return Json{{"schedule_id", optional_string_json(command.schedule_id)},
                {"display_name", command.display_name},
                {"enabled", command.enabled},
                {"protection", encode_protection_spec(command.protection)},
                {"repository_connection_id", command.repository_connection_id},
                {"backup_type", static_cast<std::uint8_t>(command.backup_type)},
                {"trigger", encode_schedule_trigger(command.trigger)},
                {"exclude_page_and_hibernation_files", command.exclude_page_and_hibernation_files},
                {"deduplication_enabled", command.deduplication_enabled},
                {"encryption_enabled", command.encryption_enabled},
                {"archive_password", command.archive_password}};
}

[[nodiscard]] contracts::UpsertScheduleCommand parse_upsert_schedule(const Json& payload) {
    constexpr std::array<std::string_view, 11> keys{
        "schedule_id",
        "display_name",
        "enabled",
        "protection",
        "repository_connection_id",
        "backup_type",
        "trigger",
        "exclude_page_and_hibernation_files",
        "deduplication_enabled",
        "encryption_enabled",
        "archive_password"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("upsert schedule fields are invalid");
    }
    contracts::UpsertScheduleCommand command;
    command.schedule_id = optional_string(payload.at("schedule_id"));
    command.display_name = payload.at("display_name").get<std::string>();
    command.enabled = payload.at("enabled").get<bool>();
    command.protection = parse_protection_spec(payload.at("protection"));
    command.repository_connection_id = payload.at("repository_connection_id").get<std::string>();
    command.backup_type =
        static_cast<contracts::BackupType>(unsigned_value<std::uint8_t>(payload, "backup_type"));
    command.trigger = parse_schedule_trigger(payload.at("trigger"));
    command.exclude_page_and_hibernation_files =
        payload.at("exclude_page_and_hibernation_files").get<bool>();
    command.deduplication_enabled = payload.at("deduplication_enabled").get<bool>();
    command.encryption_enabled = payload.at("encryption_enabled").get<bool>();
    command.archive_password = payload.at("archive_password").get<std::string>();
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
    case contracts::ServiceRequestKind::kGetRecoveryPointLayout:
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
    case contracts::ServiceRequestKind::kBrowseFileSources: {
        const auto& body = std::get<contracts::BrowseFileSourcesRequest>(request.payload);
        return Json{{"parent_node_token", optional_string_json(body.parent_node_token)},
                    {"page", encode_page_request(body.page)},
                    {"include_unavailable", body.include_unavailable}};
    }
    case contracts::ServiceRequestKind::kListRecoveryPointEntries: {
        const auto& body = std::get<contracts::ListRecoveryPointEntriesRequest>(request.payload);
        return Json{{"repository_connection_id", optional_string_json(body.repository_connection_id)},
                    {"recovery_point_id", body.recovery_point_id},
                    {"parent_entry_id", body.parent_entry_id},
                    {"page", encode_page_request(body.page)},
                    {"archive_secret_ref", optional_string_json(body.archive_secret_ref)}};
    }
    case contracts::ServiceRequestKind::kPrepareFileRestore: {
        const auto& body = std::get<contracts::PrepareFileRestoreRequest>(request.payload);
        return Json{{"repository_connection_id", optional_string_json(body.repository_connection_id)},
                    {"recovery_point_id", body.recovery_point_id},
                    {"entry_ids", body.entry_ids},
                    {"target_node_token", body.target_node_token},
                    {"conflict_policy", static_cast<std::uint8_t>(body.conflict_policy)},
                    {"archive_secret_ref", optional_string_json(body.archive_secret_ref)},
                    {"restore_security", body.restore_security}};
    }
    case contracts::ServiceRequestKind::kStartFileRestore: {
        const auto& body = std::get<contracts::StartFileRestoreCommand>(request.payload);
        return Json{{"preflight_token", body.preflight_token},
                    {"confirmed", body.confirmed},
                    {"archive_secret_ref", optional_string_json(body.archive_secret_ref)}};
    }
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
    case contracts::ServiceRequestKind::kGetRecoveryPointLayout:
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
    case contracts::ServiceRequestKind::kBrowseFileSources: {
        constexpr std::array<std::string_view, 3> keys{"parent_node_token", "page",
                                                       "include_unavailable"};
        if (!exact_keys(payload, keys)) {
            throw std::invalid_argument("browse file sources fields are invalid");
        }
        contracts::BrowseFileSourcesRequest request;
        request.parent_node_token = optional_string(payload.at("parent_node_token"));
        request.page = parse_page_request(payload.at("page"));
        request.include_unavailable = payload.at("include_unavailable").get<bool>();
        return request;
    }
    case contracts::ServiceRequestKind::kListRecoveryPointEntries: {
        constexpr std::array<std::string_view, 5> keys{
            "repository_connection_id", "recovery_point_id", "parent_entry_id", "page",
            "archive_secret_ref"};
        if (!exact_keys(payload, keys)) {
            throw std::invalid_argument("list recovery point entries fields are invalid");
        }
        contracts::ListRecoveryPointEntriesRequest request;
        request.repository_connection_id = optional_string(payload.at("repository_connection_id"));
        request.recovery_point_id = payload.at("recovery_point_id").get<std::string>();
        request.parent_entry_id = payload.at("parent_entry_id").get<std::string>();
        request.page = parse_page_request(payload.at("page"));
        request.archive_secret_ref = optional_string(payload.at("archive_secret_ref"));
        return request;
    }
    case contracts::ServiceRequestKind::kPrepareFileRestore: {
        constexpr std::array<std::string_view, 7> keys{
            "repository_connection_id", "recovery_point_id", "entry_ids", "target_node_token",
            "conflict_policy", "archive_secret_ref", "restore_security"};
        if (!exact_keys(payload, keys)) {
            throw std::invalid_argument("prepare file restore fields are invalid");
        }
        contracts::PrepareFileRestoreRequest request;
        request.repository_connection_id = optional_string(payload.at("repository_connection_id"));
        request.recovery_point_id = payload.at("recovery_point_id").get<std::string>();
        request.entry_ids = payload.at("entry_ids").get<std::vector<std::string>>();
        request.target_node_token = payload.at("target_node_token").get<std::string>();
        request.conflict_policy = static_cast<contracts::FileConflictPolicy>(
            unsigned_value<std::uint8_t>(payload, "conflict_policy"));
        request.archive_secret_ref = optional_string(payload.at("archive_secret_ref"));
        request.restore_security = payload.at("restore_security").get<bool>();
        return request;
    }
    case contracts::ServiceRequestKind::kStartFileRestore: {
        constexpr std::array<std::string_view, 3> keys{"preflight_token", "confirmed",
                                                       "archive_secret_ref"};
        if (!exact_keys(payload, keys)) {
            throw std::invalid_argument("start file restore fields are invalid");
        }
        contracts::StartFileRestoreCommand command;
        command.preflight_token = payload.at("preflight_token").get<std::string>();
        command.confirmed = payload.at("confirmed").get<bool>();
        command.archive_secret_ref = optional_string(payload.at("archive_secret_ref"));
        return command;
    }
    }
    throw std::invalid_argument("service request kind is invalid");
}

} // namespace aegra::apps::service::protocol_json
