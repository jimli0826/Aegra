#include "service_protocol_json.h"

#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aegra::apps::service::protocol_json {
namespace {

[[nodiscard]] Json encode_service_info(const contracts::ServiceInfo& service) {
    return Json{{"minimum_api_version", service.minimum_api_version},
                {"api_version", service.api_version},
                {"state", static_cast<std::uint8_t>(service.state)},
                {"service_version", service.service_version},
                {"capabilities", service.capabilities}};
}

[[nodiscard]] contracts::ServiceInfo parse_service_info(const Json& payload) {
    constexpr std::array<std::string_view, 5> keys{"minimum_api_version", "api_version", "state",
                                                   "service_version", "capabilities"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("service info fields are invalid");
    }
    contracts::ServiceInfo service;
    service.minimum_api_version = unsigned_value<std::uint32_t>(payload, "minimum_api_version");
    service.api_version = unsigned_value<std::uint32_t>(payload, "api_version");
    service.state =
        static_cast<contracts::ServiceState>(unsigned_value<std::uint8_t>(payload, "state"));
    service.service_version = payload.at("service_version").get<std::string>();
    service.capabilities = payload.at("capabilities").get<std::vector<std::string>>();
    return service;
}

[[nodiscard]] Json encode_recovery_point(const contracts::RecoveryPointSummary& point) {
    return Json{{"file_uuid", point.file_uuid},
                {"backup_set_uuid", point.backup_set_uuid},
                {"parent_uuid", optional_string_json(point.parent_uuid)},
                {"backup_type", static_cast<std::uint8_t>(point.backup_type)},
                {"chain_state", static_cast<std::uint8_t>(point.chain_state)},
                {"created_utc_ms", point.created_utc_ms},
                {"logical_size_bytes", point.logical_size_bytes},
                {"stored_size_bytes", point.stored_size_bytes},
                {"source_count", point.source_count},
                {"has_sidecar", point.has_sidecar}};
}

[[nodiscard]] contracts::RecoveryPointSummary parse_recovery_point(const Json& payload) {
    constexpr std::array<std::string_view, 10> keys{
        "file_uuid",      "backup_set_uuid",    "parent_uuid",       "backup_type",  "chain_state",
        "created_utc_ms", "logical_size_bytes", "stored_size_bytes", "source_count", "has_sidecar"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("recovery point summary fields are invalid");
    }
    contracts::RecoveryPointSummary point;
    point.file_uuid = payload.at("file_uuid").get<std::string>();
    point.backup_set_uuid = payload.at("backup_set_uuid").get<std::string>();
    point.parent_uuid = optional_string(payload.at("parent_uuid"));
    point.backup_type = static_cast<contracts::PersonalBackupType>(
        unsigned_value<std::uint8_t>(payload, "backup_type"));
    point.chain_state = static_cast<contracts::RecoveryPointChainState>(
        unsigned_value<std::uint8_t>(payload, "chain_state"));
    point.created_utc_ms = unsigned_value<std::uint64_t>(payload, "created_utc_ms");
    point.logical_size_bytes = unsigned_value<std::uint64_t>(payload, "logical_size_bytes");
    point.stored_size_bytes = unsigned_value<std::uint64_t>(payload, "stored_size_bytes");
    point.source_count = unsigned_value<std::uint32_t>(payload, "source_count");
    point.has_sidecar = payload.at("has_sidecar").get<bool>();
    return point;
}

[[nodiscard]] Json encode_recovery_point_page(const contracts::RecoveryPointPage& page) {
    Json items = Json::array();
    for (const auto& item : page.items) {
        items.push_back(encode_recovery_point(item));
    }
    return Json{{"state", static_cast<std::uint8_t>(page.state)},
                {"repository_uuid", page.repository_uuid},
                {"items", std::move(items)},
                {"continuation_token", optional_string_json(page.continuation_token)}};
}

[[nodiscard]] contracts::RecoveryPointPage parse_recovery_point_page(const Json& payload) {
    constexpr std::array<std::string_view, 4> keys{"state", "repository_uuid", "items",
                                                   "continuation_token"};
    if (!exact_keys(payload, keys) || !payload.at("items").is_array()) {
        throw std::invalid_argument("recovery point page fields are invalid");
    }
    contracts::RecoveryPointPage page;
    page.state = static_cast<contracts::RepositoryCatalogState>(
        unsigned_value<std::uint8_t>(payload, "state"));
    page.repository_uuid = payload.at("repository_uuid").get<std::string>();
    page.continuation_token = optional_string(payload.at("continuation_token"));
    for (const auto& item : payload.at("items")) {
        page.items.push_back(parse_recovery_point(item));
    }
    return page;
}

[[nodiscard]] Json
encode_service_recovery_point_page(const contracts::ServiceRecoveryPointPage& page) {
    return Json{{"repository_connection_id", optional_string_json(page.repository_connection_id)},
                {"catalog", encode_recovery_point_page(page.catalog)}};
}

[[nodiscard]] contracts::ServiceRecoveryPointPage
parse_service_recovery_point_page(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"repository_connection_id", "catalog"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("service recovery point page fields are invalid");
    }
    return {optional_string(payload.at("repository_connection_id")),
            parse_recovery_point_page(payload.at("catalog"))};
}

[[nodiscard]] Json
encode_repository_connection(const contracts::RepositoryConnectionSummary& summary) {
    return Json{{"connection_id", summary.connection_id},
                {"display_name", summary.display_name},
                {"state", static_cast<std::uint8_t>(summary.state)},
                {"is_default", summary.is_default},
                {"capabilities", summary.capabilities}};
}

[[nodiscard]] contracts::RepositoryConnectionSummary
parse_repository_connection(const Json& payload) {
    constexpr std::array<std::string_view, 5> keys{"connection_id", "display_name", "state",
                                                   "is_default", "capabilities"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("repository connection summary fields are invalid");
    }
    contracts::RepositoryConnectionSummary summary;
    summary.connection_id = payload.at("connection_id").get<std::string>();
    summary.display_name = payload.at("display_name").get<std::string>();
    summary.state = static_cast<contracts::RepositoryConnectionState>(
        unsigned_value<std::uint8_t>(payload, "state"));
    summary.is_default = payload.at("is_default").get<bool>();
    summary.capabilities = payload.at("capabilities").get<std::vector<std::string>>();
    return summary;
}

[[nodiscard]] Json encode_source(const contracts::SourceInventoryItem& item) {
    return Json{{"source_id", item.source_id},
                {"display_name", item.display_name},
                {"kind", static_cast<std::uint8_t>(item.kind)},
                {"availability", static_cast<std::uint8_t>(item.availability)},
                {"capacity_bytes", item.capacity_bytes},
                {"is_system", item.is_system},
                {"is_read_only", item.is_read_only},
                {"is_selectable", item.is_selectable}};
}

[[nodiscard]] contracts::SourceInventoryItem parse_source(const Json& payload) {
    constexpr std::array<std::string_view, 8> keys{"source_id",    "display_name",   "kind",
                                                   "availability", "capacity_bytes", "is_system",
                                                   "is_read_only", "is_selectable"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("source inventory fields are invalid");
    }
    contracts::SourceInventoryItem item;
    item.source_id = payload.at("source_id").get<std::string>();
    item.display_name = payload.at("display_name").get<std::string>();
    item.kind = static_cast<contracts::SourceKind>(unsigned_value<std::uint8_t>(payload, "kind"));
    item.availability = static_cast<contracts::SourceAvailability>(
        unsigned_value<std::uint8_t>(payload, "availability"));
    item.capacity_bytes = unsigned_value<std::uint64_t>(payload, "capacity_bytes");
    item.is_system = payload.at("is_system").get<bool>();
    item.is_read_only = payload.at("is_read_only").get<bool>();
    item.is_selectable = payload.at("is_selectable").get<bool>();
    return item;
}

[[nodiscard]] Json encode_task_progress(const contracts::TaskProgress& progress) {
    return Json{{"schema_version", progress.schema_version},
                {"job_id", progress.job_id},
                {"trace_id", progress.trace_id},
                {"phase", static_cast<std::uint8_t>(progress.phase)},
                {"logical_bytes", progress.logical_bytes},
                {"processed_bytes", progress.processed_bytes},
                {"stored_bytes", progress.stored_bytes},
                {"message_code", progress.message_code}};
}

[[nodiscard]] contracts::TaskProgress parse_task_progress(const Json& payload) {
    constexpr std::array<std::string_view, 8> keys{
        "schema_version", "job_id",          "trace_id",     "phase",
        "logical_bytes",  "processed_bytes", "stored_bytes", "message_code"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("task progress fields are invalid");
    }
    contracts::TaskProgress progress;
    progress.schema_version = unsigned_value<std::uint32_t>(payload, "schema_version");
    progress.job_id = payload.at("job_id").get<std::string>();
    progress.trace_id = payload.at("trace_id").get<std::string>();
    progress.phase =
        static_cast<contracts::TaskPhase>(unsigned_value<std::uint8_t>(payload, "phase"));
    progress.logical_bytes = unsigned_value<std::uint64_t>(payload, "logical_bytes");
    progress.processed_bytes = unsigned_value<std::uint64_t>(payload, "processed_bytes");
    progress.stored_bytes = unsigned_value<std::uint64_t>(payload, "stored_bytes");
    progress.message_code = payload.at("message_code").get<std::string>();
    return progress;
}

[[nodiscard]] Json encode_job(const contracts::JobSummary& summary) {
    return Json{
        {"job_id", summary.job_id},
        {"trace_id", summary.trace_id},
        {"operation", static_cast<std::uint8_t>(summary.operation)},
        {"state", static_cast<std::uint8_t>(summary.state)},
        {"created_utc_ms", summary.created_utc_ms},
        {"started_utc_ms", optional_uint64_json(summary.started_utc_ms)},
        {"completed_utc_ms", optional_uint64_json(summary.completed_utc_ms)},
        {"progress", summary.progress ? encode_task_progress(*summary.progress) : Json(nullptr)},
        {"message_code", summary.message_code}};
}

[[nodiscard]] contracts::JobSummary parse_job(const Json& payload) {
    constexpr std::array<std::string_view, 9> keys{
        "job_id",         "trace_id",         "operation", "state",       "created_utc_ms",
        "started_utc_ms", "completed_utc_ms", "progress",  "message_code"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("job summary fields are invalid");
    }
    contracts::JobSummary summary;
    summary.job_id = payload.at("job_id").get<std::string>();
    summary.trace_id = payload.at("trace_id").get<std::string>();
    summary.operation =
        static_cast<contracts::JobOperation>(unsigned_value<std::uint8_t>(payload, "operation"));
    summary.state =
        static_cast<contracts::ServiceJobState>(unsigned_value<std::uint8_t>(payload, "state"));
    summary.created_utc_ms = unsigned_value<std::uint64_t>(payload, "created_utc_ms");
    summary.started_utc_ms = optional_uint64(payload.at("started_utc_ms"));
    summary.completed_utc_ms = optional_uint64(payload.at("completed_utc_ms"));
    if (!payload.at("progress").is_null()) {
        summary.progress = parse_task_progress(payload.at("progress"));
    }
    summary.message_code = payload.at("message_code").get<std::string>();
    return summary;
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
    return {
        static_cast<contracts::ScheduleTriggerKind>(unsigned_value<std::uint8_t>(payload, "kind")),
        unsigned_value<std::uint16_t>(payload, "local_minute_of_day"),
        unsigned_value<std::uint8_t>(payload, "weekday_mask"),
        payload.at("timezone_id").get<std::string>()};
}

[[nodiscard]] Json encode_schedule(const contracts::ScheduleSummary& summary) {
    return Json{{"schedule_id", summary.schedule_id},
                {"display_name", summary.display_name},
                {"enabled", summary.enabled},
                {"source_id", summary.source_id},
                {"repository_connection_id", summary.repository_connection_id},
                {"backup_type", static_cast<std::uint8_t>(summary.backup_type)},
                {"trigger", encode_schedule_trigger(summary.trigger)},
                {"next_run_utc_ms", optional_uint64_json(summary.next_run_utc_ms)}};
}

[[nodiscard]] contracts::ScheduleSummary parse_schedule(const Json& payload) {
    constexpr std::array<std::string_view, 8> keys{
        "schedule_id", "display_name", "enabled",        "source_id", "repository_connection_id",
        "backup_type", "trigger",      "next_run_utc_ms"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("schedule summary fields are invalid");
    }
    contracts::ScheduleSummary summary;
    summary.schedule_id = payload.at("schedule_id").get<std::string>();
    summary.display_name = payload.at("display_name").get<std::string>();
    summary.enabled = payload.at("enabled").get<bool>();
    summary.source_id = payload.at("source_id").get<std::string>();
    summary.repository_connection_id = payload.at("repository_connection_id").get<std::string>();
    summary.backup_type =
        static_cast<contracts::BackupType>(unsigned_value<std::uint8_t>(payload, "backup_type"));
    summary.trigger = parse_schedule_trigger(payload.at("trigger"));
    summary.next_run_utc_ms = optional_uint64(payload.at("next_run_utc_ms"));
    return summary;
}

[[nodiscard]] Json encode_audit_event(const contracts::AuditEventSummary& summary) {
    return Json{{"event_id", summary.event_id},
                {"created_utc_ms", summary.created_utc_ms},
                {"severity", static_cast<std::uint8_t>(summary.severity)},
                {"message_code", summary.message_code},
                {"message_arguments", encode_message_arguments(summary.message_arguments)},
                {"correlation_id", summary.correlation_id}};
}

[[nodiscard]] contracts::AuditEventSummary parse_audit_event(const Json& payload) {
    constexpr std::array<std::string_view, 6> keys{"event_id",          "created_utc_ms",
                                                   "severity",          "message_code",
                                                   "message_arguments", "correlation_id"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("audit event fields are invalid");
    }
    contracts::AuditEventSummary summary;
    summary.event_id = payload.at("event_id").get<std::string>();
    summary.created_utc_ms = unsigned_value<std::uint64_t>(payload, "created_utc_ms");
    summary.severity =
        static_cast<contracts::AuditSeverity>(unsigned_value<std::uint8_t>(payload, "severity"));
    summary.message_code = payload.at("message_code").get<std::string>();
    summary.message_arguments = parse_message_arguments(payload.at("message_arguments"));
    summary.correlation_id = payload.at("correlation_id").get<std::string>();
    return summary;
}

[[nodiscard]] Json encode_mount_session(const contracts::MountSessionSummary& summary) {
    return Json{{"session_id", summary.session_id},
                {"recovery_point_id", summary.recovery_point_id},
                {"state", static_cast<std::uint8_t>(summary.state)},
                {"mount_point", summary.mount_point},
                {"started_utc_ms", summary.started_utc_ms},
                {"message_code", summary.message_code}};
}

[[nodiscard]] contracts::MountSessionSummary parse_mount_session(const Json& payload) {
    constexpr std::array<std::string_view, 6> keys{"session_id",     "recovery_point_id",
                                                   "state",          "mount_point",
                                                   "started_utc_ms", "message_code"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("mount session fields are invalid");
    }
    contracts::MountSessionSummary summary;
    summary.session_id = payload.at("session_id").get<std::string>();
    summary.recovery_point_id = payload.at("recovery_point_id").get<std::string>();
    summary.state =
        static_cast<contracts::MountSessionState>(unsigned_value<std::uint8_t>(payload, "state"));
    summary.mount_point = payload.at("mount_point").get<std::string>();
    summary.started_utc_ms = unsigned_value<std::uint64_t>(payload, "started_utc_ms");
    summary.message_code = payload.at("message_code").get<std::string>();
    return summary;
}

template <typename Item, typename Encoder>
[[nodiscard]] Json encode_page(const contracts::ServicePage<Item>& page, Encoder encoder) {
    Json items = Json::array();
    for (const auto& item : page.items) {
        items.push_back(encoder(item));
    }
    return Json{{"items", std::move(items)},
                {"continuation_token", optional_string_json(page.continuation_token)}};
}

template <typename Item, typename Parser>
[[nodiscard]] contracts::ServicePage<Item> parse_page(const Json& payload, Parser parser) {
    constexpr std::array<std::string_view, 2> keys{"items", "continuation_token"};
    if (!exact_keys(payload, keys) || !payload.at("items").is_array()) {
        throw std::invalid_argument("service page fields are invalid");
    }
    contracts::ServicePage<Item> page;
    page.continuation_token = optional_string(payload.at("continuation_token"));
    for (const auto& item : payload.at("items")) {
        page.items.push_back(parser(item));
    }
    return page;
}

[[nodiscard]] Json encode_restore_preflight(const contracts::RestorePreflight& preflight) {
    return Json{{"preflight_token", preflight.preflight_token},
                {"recovery_point_id", preflight.recovery_point_id},
                {"target_source_id", preflight.target_source_id},
                {"logical_size_bytes", preflight.logical_size_bytes},
                {"chain_depth", preflight.chain_depth},
                {"expires_utc_ms", preflight.expires_utc_ms}};
}

[[nodiscard]] contracts::RestorePreflight parse_restore_preflight(const Json& payload) {
    constexpr std::array<std::string_view, 6> keys{"preflight_token",  "recovery_point_id",
                                                   "target_source_id", "logical_size_bytes",
                                                   "chain_depth",      "expires_utc_ms"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("restore preflight fields are invalid");
    }
    return {payload.at("preflight_token").get<std::string>(),
            payload.at("recovery_point_id").get<std::string>(),
            payload.at("target_source_id").get<std::string>(),
            unsigned_value<std::uint64_t>(payload, "logical_size_bytes"),
            unsigned_value<std::uint32_t>(payload, "chain_depth"),
            unsigned_value<std::uint64_t>(payload, "expires_utc_ms")};
}

[[nodiscard]] Json encode_chain_layer(const contracts::RecoveryPointChainLayer& layer) {
    return Json{{"recovery_point_id", layer.recovery_point_id},
                {"backup_type", static_cast<std::uint8_t>(layer.backup_type)},
                {"parent_recovery_point_id", optional_string_json(layer.parent_recovery_point_id)},
                {"structural_state", static_cast<std::uint8_t>(layer.structural_state)},
                {"authentication_state", static_cast<std::uint8_t>(layer.authentication_state)},
                {"chain_state", static_cast<std::uint8_t>(layer.chain_state)}};
}

[[nodiscard]] contracts::RecoveryPointChainLayer parse_chain_layer(const Json& payload) {
    constexpr std::array<std::string_view, 6> keys{
        "recovery_point_id", "backup_type",         "parent_recovery_point_id",
        "structural_state",  "authentication_state", "chain_state"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("recovery point chain layer fields are invalid");
    }
    contracts::RecoveryPointChainLayer layer;
    layer.recovery_point_id = payload.at("recovery_point_id").get<std::string>();
    layer.backup_type =
        static_cast<contracts::BackupType>(unsigned_value<std::uint8_t>(payload, "backup_type"));
    layer.parent_recovery_point_id = optional_string(payload.at("parent_recovery_point_id"));
    layer.structural_state = static_cast<contracts::RecoveryPointStructuralState>(
        unsigned_value<std::uint8_t>(payload, "structural_state"));
    layer.authentication_state = static_cast<contracts::RecoveryPointAuthenticationState>(
        unsigned_value<std::uint8_t>(payload, "authentication_state"));
    layer.chain_state = static_cast<contracts::RecoveryPointChainCompleteness>(
        unsigned_value<std::uint8_t>(payload, "chain_state"));
    return layer;
}

[[nodiscard]] Json encode_chain_result(const contracts::RecoveryPointChainResult& result) {
    Json layers = Json::array();
    for (const auto& layer : result.layers) {
        layers.push_back(encode_chain_layer(layer));
    }
    return Json{{"repository_connection_id", result.repository_connection_id},
                {"recovery_point_id", result.recovery_point_id},
                {"layers", std::move(layers)},
                {"restore_eligible", result.restore_eligible},
                {"mount_eligible", result.mount_eligible},
                {"verify_eligible", result.verify_eligible},
                {"message_code", result.message_code}};
}

[[nodiscard]] contracts::RecoveryPointChainResult parse_chain_result(const Json& payload) {
    constexpr std::array<std::string_view, 7> keys{
        "repository_connection_id", "recovery_point_id", "layers",        "restore_eligible",
        "mount_eligible",           "verify_eligible",   "message_code"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("recovery point chain result fields are invalid");
    }
    contracts::RecoveryPointChainResult result;
    result.repository_connection_id = payload.at("repository_connection_id").get<std::string>();
    result.recovery_point_id = payload.at("recovery_point_id").get<std::string>();
    for (const auto& layer : payload.at("layers")) {
        result.layers.push_back(parse_chain_layer(layer));
    }
    result.restore_eligible = payload.at("restore_eligible").get<bool>();
    result.mount_eligible = payload.at("mount_eligible").get<bool>();
    result.verify_eligible = payload.at("verify_eligible").get<bool>();
    result.message_code = payload.at("message_code").get<std::string>();
    return result;
}

[[nodiscard]] Json encode_delete_plan_target(const contracts::DeletePlanTargetSummary& target) {
    return Json{{"recovery_point_id", target.recovery_point_id},
                {"catalog_generation", target.catalog_generation},
                {"member_count", target.member_count}};
}

[[nodiscard]] contracts::DeletePlanTargetSummary parse_delete_plan_target(const Json& payload) {
    constexpr std::array<std::string_view, 3> keys{"recovery_point_id", "catalog_generation",
                                                   "member_count"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("delete plan target fields are invalid");
    }
    return {payload.at("recovery_point_id").get<std::string>(),
            unsigned_value<std::uint64_t>(payload, "catalog_generation"),
            unsigned_value<std::uint32_t>(payload, "member_count")};
}

[[nodiscard]] Json encode_delete_plan_summary(const contracts::DeletePlanSummary& summary) {
    Json targets = Json::array();
    for (const auto& target : summary.targets) {
        targets.push_back(encode_delete_plan_target(target));
    }
    return Json{{"plan_token", summary.plan_token},
                {"operation_id", summary.operation_id},
                {"repository_connection_id", summary.repository_connection_id},
                {"root_recovery_point_id", summary.root_recovery_point_id},
                {"targets", std::move(targets)},
                {"expires_utc_ms", summary.expires_utc_ms}};
}

[[nodiscard]] contracts::DeletePlanSummary parse_delete_plan_summary(const Json& payload) {
    constexpr std::array<std::string_view, 6> keys{
        "plan_token", "operation_id", "repository_connection_id",
        "root_recovery_point_id", "targets", "expires_utc_ms"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("delete plan summary fields are invalid");
    }
    contracts::DeletePlanSummary summary;
    summary.plan_token = payload.at("plan_token").get<std::string>();
    summary.operation_id = payload.at("operation_id").get<std::string>();
    summary.repository_connection_id = payload.at("repository_connection_id").get<std::string>();
    summary.root_recovery_point_id = payload.at("root_recovery_point_id").get<std::string>();
    for (const auto& target : payload.at("targets")) {
        summary.targets.push_back(parse_delete_plan_target(target));
    }
    summary.expires_utc_ms = unsigned_value<std::uint64_t>(payload, "expires_utc_ms");
    return summary;
}

[[nodiscard]] Json encode_event_lease(const contracts::EventSubscriptionLease& lease) {
    return Json{{"subscription_id", lease.subscription_id},
                {"resume_token", lease.resume_token},
                {"next_sequence", lease.next_sequence},
                {"maximum_unacknowledged_events", lease.maximum_unacknowledged_events}};
}

[[nodiscard]] contracts::EventSubscriptionLease parse_event_lease(const Json& payload) {
    constexpr std::array<std::string_view, 4> keys{
        "subscription_id", "resume_token", "next_sequence", "maximum_unacknowledged_events"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("event subscription lease fields are invalid");
    }
    return {payload.at("subscription_id").get<std::string>(),
            payload.at("resume_token").get<std::string>(),
            unsigned_value<std::uint64_t>(payload, "next_sequence"),
            unsigned_value<std::uint32_t>(payload, "maximum_unacknowledged_events")};
}

[[nodiscard]] Json encode_command_ack(const contracts::CommandAcknowledgement& acknowledgement) {
    return Json{{"command_id", acknowledgement.command_id},
                {"disposition", static_cast<std::uint8_t>(acknowledgement.disposition)},
                {"resource_id", optional_string_json(acknowledgement.resource_id)},
                {"event_subscription", acknowledgement.event_subscription
                                           ? encode_event_lease(*acknowledgement.event_subscription)
                                           : Json(nullptr)}};
}

[[nodiscard]] contracts::CommandAcknowledgement parse_command_ack(const Json& payload) {
    constexpr std::array<std::string_view, 4> keys{"command_id", "disposition", "resource_id",
                                                   "event_subscription"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("command acknowledgement fields are invalid");
    }
    contracts::CommandAcknowledgement acknowledgement;
    acknowledgement.command_id = payload.at("command_id").get<std::string>();
    acknowledgement.disposition = static_cast<contracts::CommandDisposition>(
        unsigned_value<std::uint8_t>(payload, "disposition"));
    acknowledgement.resource_id = optional_string(payload.at("resource_id"));
    if (!payload.at("event_subscription").is_null()) {
        acknowledgement.event_subscription = parse_event_lease(payload.at("event_subscription"));
    }
    return acknowledgement;
}

[[nodiscard]] Json encode_task_result(const contracts::TaskResult& result) {
    return Json{{"schema_version", result.schema_version},
                {"job_id", result.job_id},
                {"trace_id", result.trace_id},
                {"outcome", static_cast<std::uint8_t>(result.outcome)},
                {"error_code", static_cast<std::uint32_t>(result.error_code)},
                {"logical_bytes", result.logical_bytes},
                {"stored_bytes", result.stored_bytes},
                {"chunk_count", result.chunk_count},
                {"message_code", result.message_code},
                {"warning_codes", result.warning_codes}};
}

[[nodiscard]] contracts::TaskResult parse_task_result(const Json& payload) {
    constexpr std::array<std::string_view, 10> keys{
        "schema_version", "job_id",       "trace_id",    "outcome",      "error_code",
        "logical_bytes",  "stored_bytes", "chunk_count", "message_code", "warning_codes"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("task result fields are invalid");
    }
    contracts::TaskResult result;
    result.schema_version = unsigned_value<std::uint32_t>(payload, "schema_version");
    result.job_id = payload.at("job_id").get<std::string>();
    result.trace_id = payload.at("trace_id").get<std::string>();
    result.outcome =
        static_cast<contracts::TaskOutcome>(unsigned_value<std::uint8_t>(payload, "outcome"));
    result.error_code =
        static_cast<base::ErrorCode>(unsigned_value<std::uint32_t>(payload, "error_code"));
    result.logical_bytes = unsigned_value<std::uint64_t>(payload, "logical_bytes");
    result.stored_bytes = unsigned_value<std::uint64_t>(payload, "stored_bytes");
    result.chunk_count = unsigned_value<std::uint64_t>(payload, "chunk_count");
    result.message_code = payload.at("message_code").get<std::string>();
    result.warning_codes = payload.at("warning_codes").get<std::vector<std::string>>();
    return result;
}

} // namespace

Json encode_response_payload(const contracts::ServiceResponse& response) {
    if (response.kind == contracts::ServiceResponseKind::kRequestFailed) {
        return Json(nullptr);
    }
    if (response.kind == contracts::ServiceResponseKind::kCommandAccepted) {
        return encode_command_ack(std::get<contracts::CommandAcknowledgement>(response.payload));
    }
    switch (response.request_kind) {
    case contracts::ServiceRequestKind::kGetServiceInfo:
        return encode_service_info(std::get<contracts::ServiceInfo>(response.payload));
    case contracts::ServiceRequestKind::kListRecoveryPoints:
        return encode_service_recovery_point_page(
            std::get<contracts::ServiceRecoveryPointPage>(response.payload));
    case contracts::ServiceRequestKind::kListRepositoryConnections:
        return encode_page(std::get<contracts::RepositoryConnectionPage>(response.payload),
                           encode_repository_connection);
    case contracts::ServiceRequestKind::kListSourceInventory:
        return encode_page(std::get<contracts::SourceInventoryPage>(response.payload),
                           encode_source);
    case contracts::ServiceRequestKind::kListJobs:
        return encode_page(std::get<contracts::JobPage>(response.payload), encode_job);
    case contracts::ServiceRequestKind::kListSchedules:
        return encode_page(std::get<contracts::SchedulePage>(response.payload), encode_schedule);
    case contracts::ServiceRequestKind::kListEvents:
        return encode_page(std::get<contracts::AuditEventPage>(response.payload),
                           encode_audit_event);
    case contracts::ServiceRequestKind::kListMountSessions:
        return encode_page(std::get<contracts::MountSessionPage>(response.payload),
                           encode_mount_session);
    case contracts::ServiceRequestKind::kPrepareRestore:
        return encode_restore_preflight(std::get<contracts::RestorePreflight>(response.payload));
    case contracts::ServiceRequestKind::kResolveRecoveryPointChain:
        return encode_chain_result(std::get<contracts::RecoveryPointChainResult>(response.payload));
    case contracts::ServiceRequestKind::kPlanDeleteRecoveryPoints:
        return encode_delete_plan_summary(std::get<contracts::DeletePlanSummary>(response.payload));
    default:
        throw std::invalid_argument("service query response kind is invalid");
    }
}

contracts::ServiceResponsePayload
parse_response_payload(const contracts::ServiceResponseKind response_kind,
                       const contracts::ServiceRequestKind request_kind, const Json& payload) {
    if (response_kind == contracts::ServiceResponseKind::kRequestFailed) {
        if (!payload.is_null()) {
            throw std::invalid_argument("service failure payload is invalid");
        }
        return std::monostate{};
    }
    if (response_kind == contracts::ServiceResponseKind::kCommandAccepted) {
        return parse_command_ack(payload);
    }
    if (response_kind != contracts::ServiceResponseKind::kQueryResult) {
        throw std::invalid_argument("service response kind is invalid");
    }
    switch (request_kind) {
    case contracts::ServiceRequestKind::kGetServiceInfo:
        return parse_service_info(payload);
    case contracts::ServiceRequestKind::kListRecoveryPoints:
        return parse_service_recovery_point_page(payload);
    case contracts::ServiceRequestKind::kListRepositoryConnections:
        return parse_page<contracts::RepositoryConnectionSummary>(payload,
                                                                  parse_repository_connection);
    case contracts::ServiceRequestKind::kListSourceInventory:
        return parse_page<contracts::SourceInventoryItem>(payload, parse_source);
    case contracts::ServiceRequestKind::kListJobs:
        return parse_page<contracts::JobSummary>(payload, parse_job);
    case contracts::ServiceRequestKind::kListSchedules:
        return parse_page<contracts::ScheduleSummary>(payload, parse_schedule);
    case contracts::ServiceRequestKind::kListEvents:
        return parse_page<contracts::AuditEventSummary>(payload, parse_audit_event);
    case contracts::ServiceRequestKind::kListMountSessions:
        return parse_page<contracts::MountSessionSummary>(payload, parse_mount_session);
    case contracts::ServiceRequestKind::kPrepareRestore:
        return parse_restore_preflight(payload);
    case contracts::ServiceRequestKind::kResolveRecoveryPointChain:
        return parse_chain_result(payload);
    case contracts::ServiceRequestKind::kPlanDeleteRecoveryPoints:
        return parse_delete_plan_summary(payload);
    default:
        throw std::invalid_argument("service query response kind is invalid");
    }
}

Json encode_event_payload(const contracts::ServiceEvent& event) {
    switch (event.kind) {
    case contracts::ServiceEventKind::kTaskProgress:
        return encode_task_progress(std::get<contracts::TaskProgress>(event.payload));
    case contracts::ServiceEventKind::kTaskCompleted:
        return encode_task_result(std::get<contracts::TaskResult>(event.payload));
    case contracts::ServiceEventKind::kMountSessionChanged:
        return encode_mount_session(std::get<contracts::MountSessionSummary>(event.payload));
    }
    throw std::invalid_argument("service event kind is invalid");
}

contracts::ServiceEventPayload parse_event_payload(const contracts::ServiceEventKind kind,
                                                   const Json& payload) {
    switch (kind) {
    case contracts::ServiceEventKind::kTaskProgress:
        return parse_task_progress(payload);
    case contracts::ServiceEventKind::kTaskCompleted:
        return parse_task_result(payload);
    case contracts::ServiceEventKind::kMountSessionChanged:
        return parse_mount_session(payload);
    }
    throw std::invalid_argument("service event kind is invalid");
}

} // namespace aegra::apps::service::protocol_json
