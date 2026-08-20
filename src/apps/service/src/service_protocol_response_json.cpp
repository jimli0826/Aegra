#include "service_protocol_json.h"

#include "aegra/contracts/service_control.h"

#include <algorithm>
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
                {"content_kind", static_cast<std::uint8_t>(point.content_kind)},
                {"chain_state", static_cast<std::uint8_t>(point.chain_state)},
                {"created_utc_ms", point.created_utc_ms},
                {"logical_size_bytes", point.logical_size_bytes},
                {"stored_size_bytes", point.stored_size_bytes},
                {"deduplicated_block_count", point.deduplicated_block_count},
                {"deduplicated_logical_bytes", point.deduplicated_logical_bytes},
                {"source_count", point.source_count},
                {"has_sidecar", point.has_sidecar}};
}

[[nodiscard]] contracts::RecoveryPointSummary parse_recovery_point(const Json& payload) {
    constexpr std::array<std::string_view, 13> keys{"file_uuid",
                                                    "backup_set_uuid",
                                                    "parent_uuid",
                                                    "backup_type",
                                                    "content_kind",
                                                    "chain_state",
                                                    "created_utc_ms",
                                                    "logical_size_bytes",
                                                    "stored_size_bytes",
                                                    "deduplicated_block_count",
                                                    "deduplicated_logical_bytes",
                                                    "source_count",
                                                    "has_sidecar"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("recovery point summary fields are invalid");
    }
    contracts::RecoveryPointSummary point;
    point.file_uuid = payload.at("file_uuid").get<std::string>();
    point.backup_set_uuid = payload.at("backup_set_uuid").get<std::string>();
    point.parent_uuid = optional_string(payload.at("parent_uuid"));
    point.backup_type = static_cast<contracts::PersonalBackupType>(
        unsigned_value<std::uint8_t>(payload, "backup_type"));
    point.content_kind =
        static_cast<contracts::ContentKind>(unsigned_value<std::uint8_t>(payload, "content_kind"));
    point.chain_state = static_cast<contracts::RecoveryPointChainState>(
        unsigned_value<std::uint8_t>(payload, "chain_state"));
    point.created_utc_ms = unsigned_value<std::uint64_t>(payload, "created_utc_ms");
    point.logical_size_bytes = unsigned_value<std::uint64_t>(payload, "logical_size_bytes");
    point.stored_size_bytes = unsigned_value<std::uint64_t>(payload, "stored_size_bytes");
    point.deduplicated_block_count =
        unsigned_value<std::uint64_t>(payload, "deduplicated_block_count");
    point.deduplicated_logical_bytes =
        unsigned_value<std::uint64_t>(payload, "deduplicated_logical_bytes");
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
                {"locator", summary.locator},
                {"state", static_cast<std::uint8_t>(summary.state)},
                {"is_default", summary.is_default},
                {"capabilities", summary.capabilities}};
}

[[nodiscard]] contracts::RepositoryConnectionSummary
parse_repository_connection(const Json& payload) {
    constexpr std::array<std::string_view, 6> keys{"connection_id", "display_name", "locator",
                                                   "state",         "is_default",   "capabilities"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("repository connection summary fields are invalid");
    }
    contracts::RepositoryConnectionSummary summary;
    summary.connection_id = payload.at("connection_id").get<std::string>();
    summary.display_name = payload.at("display_name").get<std::string>();
    summary.locator = payload.at("locator").get<std::string>();
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
                {"free_bytes", item.free_bytes},
                {"disk_capacity_bytes", item.disk_capacity_bytes},
                {"is_system", item.is_system},
                {"is_read_only", item.is_read_only},
                {"is_selectable", item.is_selectable},
                {"disk_number", item.disk_number},
                {"offset_bytes", item.offset_bytes},
                {"mount_letter", item.mount_letter},
                {"volume_label", item.volume_label},
                {"health_status", item.health_status},
                {"partition_style", item.partition_style},
                {"media_type", item.media_type}};
}

[[nodiscard]] contracts::SourceInventoryItem parse_source(const Json& payload) {
    constexpr std::array<std::string_view, 17> keys{
        "source_id",   "display_name",        "kind",          "availability",  "capacity_bytes",
        "free_bytes",  "disk_capacity_bytes", "is_system",     "is_read_only",  "is_selectable",
        "disk_number", "offset_bytes",        "mount_letter",  "volume_label",  "health_status",
        "partition_style", "media_type"};
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
    item.free_bytes = unsigned_value<std::uint64_t>(payload, "free_bytes");
    item.disk_capacity_bytes = unsigned_value<std::uint64_t>(payload, "disk_capacity_bytes");
    item.is_system = payload.at("is_system").get<bool>();
    item.is_read_only = payload.at("is_read_only").get<bool>();
    item.is_selectable = payload.at("is_selectable").get<bool>();
    item.disk_number = unsigned_value<std::uint32_t>(payload, "disk_number");
    item.offset_bytes = unsigned_value<std::uint64_t>(payload, "offset_bytes");
    item.mount_letter = payload.at("mount_letter").get<std::string>();
    item.volume_label = payload.at("volume_label").get<std::string>();
    item.health_status = payload.at("health_status").get<std::string>();
    item.partition_style = payload.at("partition_style").get<std::string>();
    item.media_type = payload.at("media_type").get<std::string>();
    return item;
}

[[nodiscard]] Json optional_content_kind_json(const std::optional<contracts::ContentKind>& kind) {
    return kind ? Json(static_cast<std::uint8_t>(*kind)) : Json(nullptr);
}

[[nodiscard]] std::optional<contracts::ContentKind> optional_content_kind(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_number_unsigned()) {
        throw std::invalid_argument("content_kind is invalid");
    }
    const auto decoded = value.get<std::uint64_t>();
    if (decoded > std::numeric_limits<std::uint8_t>::max()) {
        throw std::out_of_range("content_kind is out of range");
    }
    return static_cast<contracts::ContentKind>(static_cast<std::uint8_t>(decoded));
}

[[nodiscard]] Json optional_wire_uint64_json(const std::optional<std::uint64_t>& value) {
    return value ? Json(*value) : Json(nullptr);
}

[[nodiscard]] std::optional<std::uint64_t> optional_wire_uint64(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_number_unsigned()) {
        throw std::invalid_argument("optional wire integer is invalid");
    }
    const auto decoded = value.get<std::uint64_t>();
    if (decoded > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        throw std::out_of_range("optional wire integer is out of range");
    }
    return decoded;
}

[[nodiscard]] Json encode_partial_restore(const contracts::PartialRestoreStats& stats) {
    return Json{{"entries_requested", stats.entries_requested},
                {"entries_restored", stats.entries_restored},
                {"entries_failed", stats.entries_failed},
                {"bytes_restored", stats.bytes_restored},
                {"stable_error_codes", stats.stable_error_codes}};
}

[[nodiscard]] contracts::PartialRestoreStats parse_partial_restore(const Json& payload) {
    constexpr std::array<std::string_view, 5> keys{"entries_requested", "entries_restored",
                                                   "entries_failed", "bytes_restored",
                                                   "stable_error_codes"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("partial restore stats fields are invalid");
    }
    contracts::PartialRestoreStats stats;
    stats.entries_requested = unsigned_value<std::uint64_t>(payload, "entries_requested");
    stats.entries_restored = unsigned_value<std::uint64_t>(payload, "entries_restored");
    stats.entries_failed = unsigned_value<std::uint64_t>(payload, "entries_failed");
    stats.bytes_restored = unsigned_value<std::uint64_t>(payload, "bytes_restored");
    stats.stable_error_codes = payload.at("stable_error_codes").get<std::vector<std::string>>();
    return stats;
}

[[nodiscard]] Json encode_task_progress(const contracts::TaskProgress& progress) {
    return Json{{"schema_version", progress.schema_version},
                {"job_id", progress.job_id},
                {"trace_id", progress.trace_id},
                {"phase", static_cast<std::uint8_t>(progress.phase)},
                {"logical_bytes", optional_wire_uint64_json(progress.logical_bytes)},
                {"processed_bytes", progress.processed_bytes},
                {"stored_bytes", progress.stored_bytes},
                {"discovered_entries", progress.discovered_entries},
                {"processed_entries", progress.processed_entries},
                {"message_code", progress.message_code}};
}

[[nodiscard]] contracts::TaskProgress parse_task_progress(const Json& payload) {
    constexpr std::array<std::string_view, 10> keys{
        "schema_version",    "job_id",          "trace_id",     "phase",
        "logical_bytes",     "processed_bytes", "stored_bytes", "discovered_entries",
        "processed_entries", "message_code"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("task progress fields are invalid");
    }
    contracts::TaskProgress progress;
    progress.schema_version = unsigned_value<std::uint32_t>(payload, "schema_version");
    progress.job_id = payload.at("job_id").get<std::string>();
    progress.trace_id = payload.at("trace_id").get<std::string>();
    progress.phase =
        static_cast<contracts::TaskPhase>(unsigned_value<std::uint8_t>(payload, "phase"));
    progress.logical_bytes = optional_wire_uint64(payload.at("logical_bytes"));
    progress.processed_bytes = unsigned_value<std::uint64_t>(payload, "processed_bytes");
    progress.stored_bytes = unsigned_value<std::uint64_t>(payload, "stored_bytes");
    progress.discovered_entries = unsigned_value<std::uint64_t>(payload, "discovered_entries");
    progress.processed_entries = unsigned_value<std::uint64_t>(payload, "processed_entries");
    progress.message_code = payload.at("message_code").get<std::string>();
    return progress;
}

[[nodiscard]] Json encode_job(const contracts::JobSummary& summary) {
    return Json{
        {"job_id", summary.job_id},
        {"trace_id", summary.trace_id},
        {"operation", static_cast<std::uint8_t>(summary.operation)},
        {"state", static_cast<std::uint8_t>(summary.state)},
        {"content_kind", optional_content_kind_json(summary.content_kind)},
        {"created_utc_ms", summary.created_utc_ms},
        {"started_utc_ms", optional_uint64_json(summary.started_utc_ms)},
        {"completed_utc_ms", optional_uint64_json(summary.completed_utc_ms)},
        {"progress", summary.progress ? encode_task_progress(*summary.progress) : Json(nullptr)},
        {"message_code", summary.message_code},
        {"source_ids", summary.source_ids},
        {"schedule_id", optional_string_json(summary.schedule_id)},
        {"repository_connection_id", optional_string_json(summary.repository_connection_id)},
        {"requested_backup_type",
         summary.requested_backup_type ? Json(*summary.requested_backup_type) : Json(nullptr)},
        {"effective_backup_type",
         summary.effective_backup_type ? Json(*summary.effective_backup_type) : Json(nullptr)},
        {"effective_parent_uuid", optional_string_json(summary.effective_parent_uuid)},
        {"incremental_downgrade_reason", summary.incremental_downgrade_reason
                                             ? Json(*summary.incremental_downgrade_reason)
                                             : Json(nullptr)}};
}

[[nodiscard]] contracts::JobSummary parse_job(const Json& payload) {
    constexpr std::array<std::string_view, 17> keys{"job_id",
                                                    "trace_id",
                                                    "operation",
                                                    "state",
                                                    "content_kind",
                                                    "created_utc_ms",
                                                    "started_utc_ms",
                                                    "completed_utc_ms",
                                                    "progress",
                                                    "message_code",
                                                    "source_ids",
                                                    "schedule_id",
                                                    "repository_connection_id",
                                                    "requested_backup_type",
                                                    "effective_backup_type",
                                                    "effective_parent_uuid",
                                                    "incremental_downgrade_reason"};
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
    summary.content_kind = optional_content_kind(payload.at("content_kind"));
    summary.created_utc_ms = unsigned_value<std::uint64_t>(payload, "created_utc_ms");
    summary.started_utc_ms = optional_uint64(payload.at("started_utc_ms"));
    summary.completed_utc_ms = optional_uint64(payload.at("completed_utc_ms"));
    if (!payload.at("progress").is_null()) {
        summary.progress = parse_task_progress(payload.at("progress"));
    }
    summary.message_code = payload.at("message_code").get<std::string>();
    summary.source_ids = payload.at("source_ids").get<std::vector<std::string>>();
    summary.schedule_id = optional_string(payload.at("schedule_id"));
    summary.repository_connection_id = optional_string(payload.at("repository_connection_id"));
    if (!payload.at("requested_backup_type").is_null()) {
        summary.requested_backup_type =
            unsigned_value<std::uint8_t>(payload, "requested_backup_type");
    }
    if (!payload.at("effective_backup_type").is_null()) {
        summary.effective_backup_type =
            unsigned_value<std::uint8_t>(payload, "effective_backup_type");
    }
    summary.effective_parent_uuid = optional_string(payload.at("effective_parent_uuid"));
    if (!payload.at("incremental_downgrade_reason").is_null()) {
        const auto reason = static_cast<contracts::IncrementalDowngradeReason>(
            unsigned_value<std::uint8_t>(payload, "incremental_downgrade_reason"));
        if (!contracts::is_known_incremental_downgrade_reason(reason) ||
            reason == contracts::IncrementalDowngradeReason::kNone) {
            throw std::invalid_argument("job summary incremental downgrade reason is invalid");
        }
        summary.incremental_downgrade_reason = static_cast<std::uint8_t>(reason);
    }
    return summary;
}

[[nodiscard]] Json
encode_recovery_point_source_partition(const contracts::RecoveryPointSourcePartition& partition) {
    return Json{{"partition_number", partition.partition_number},
                {"offset_bytes", partition.offset_bytes},
                {"size_bytes", partition.size_bytes},
                {"is_active", partition.is_active},
                {"mbr_type", partition.mbr_type},
                {"gpt_type_guid", partition.gpt_type_guid},
                {"gpt_name", partition.gpt_name},
                {"volume_label", partition.volume_label},
                {"filesystem", partition.filesystem}};
}

[[nodiscard]] contracts::RecoveryPointSourcePartition
parse_recovery_point_source_partition(const Json& payload) {
    constexpr std::array<std::string_view, 9> keys{
        "partition_number", "offset_bytes", "size_bytes",   "is_active", "mbr_type",
        "gpt_type_guid",    "gpt_name",     "volume_label", "filesystem"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("recovery point source partition fields are invalid");
    }
    return {unsigned_value<std::uint32_t>(payload, "partition_number"),
            unsigned_value<std::uint64_t>(payload, "offset_bytes"),
            unsigned_value<std::uint64_t>(payload, "size_bytes"),
            payload.at("is_active").get<bool>(),
            unsigned_value<std::uint8_t>(payload, "mbr_type"),
            payload.at("gpt_type_guid").get<std::string>(),
            payload.at("gpt_name").get<std::string>(),
            payload.at("volume_label").get<std::string>(),
            payload.at("filesystem").get<std::string>()};
}

[[nodiscard]] Json
encode_recovery_point_source_disk(const contracts::RecoveryPointSourceDisk& disk) {
    Json partitions = Json::array();
    for (const auto& partition : disk.partitions) {
        partitions.push_back(encode_recovery_point_source_partition(partition));
    }
    return Json{
        {"disk_number", disk.disk_number},         {"disk_size_bytes", disk.disk_size_bytes},
        {"partition_style", disk.partition_style}, {"model", disk.model},
        {"media_type", disk.media_type},           {"partitions", std::move(partitions)}};
}

[[nodiscard]] contracts::RecoveryPointSourceDisk
parse_recovery_point_source_disk(const Json& payload) {
    constexpr std::array<std::string_view, 6> keys{
        "disk_number", "disk_size_bytes", "partition_style", "model", "media_type", "partitions"};
    if (!exact_keys(payload, keys) || !payload.at("partitions").is_array()) {
        throw std::invalid_argument("recovery point source disk fields are invalid");
    }
    contracts::RecoveryPointSourceDisk disk;
    disk.disk_number = unsigned_value<std::uint32_t>(payload, "disk_number");
    disk.disk_size_bytes = unsigned_value<std::uint64_t>(payload, "disk_size_bytes");
    disk.partition_style = payload.at("partition_style").get<std::string>();
    disk.model = payload.at("model").get<std::string>();
    disk.media_type = payload.at("media_type").get<std::string>();
    for (const auto& item : payload.at("partitions")) {
        disk.partitions.push_back(parse_recovery_point_source_partition(item));
    }
    return disk;
}

[[nodiscard]] Json
encode_recovery_point_source_extent(const contracts::RecoveryPointSourceExtent& extent) {
    return Json{{"disk_number", extent.disk_number},
                {"partition_number", extent.partition_number},
                {"physical_offset", extent.physical_offset},
                {"volume_offset", extent.volume_offset},
                {"length", extent.length}};
}

[[nodiscard]] contracts::RecoveryPointSourceExtent
parse_recovery_point_source_extent(const Json& payload) {
    constexpr std::array<std::string_view, 5> keys{"disk_number", "partition_number",
                                                   "physical_offset", "volume_offset", "length"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("recovery point source extent fields are invalid");
    }
    return {unsigned_value<std::uint32_t>(payload, "disk_number"),
            unsigned_value<std::uint32_t>(payload, "partition_number"),
            unsigned_value<std::uint64_t>(payload, "physical_offset"),
            unsigned_value<std::uint64_t>(payload, "volume_offset"),
            unsigned_value<std::uint64_t>(payload, "length")};
}

[[nodiscard]] Json
encode_recovery_point_source_volume(const contracts::RecoveryPointSourceVolume& volume) {
    Json extents = Json::array();
    for (const auto& extent : volume.extents) {
        extents.push_back(encode_recovery_point_source_extent(extent));
    }
    return Json{{"volume_index", volume.volume_index},
                {"letter", volume.letter},
                {"label", volume.label},
                {"filesystem", volume.filesystem},
                {"total_size_bytes", volume.total_size_bytes},
                {"free_size_bytes", volume.free_size_bytes},
                {"free_size_known", volume.free_size_known},
                {"extents", std::move(extents)}};
}

[[nodiscard]] contracts::RecoveryPointSourceVolume
parse_recovery_point_source_volume(const Json& payload) {
    constexpr std::array<std::string_view, 8> keys{
        "volume_index",     "letter",           "label",           "filesystem",
        "total_size_bytes", "free_size_bytes",  "free_size_known", "extents"};
    if (!exact_keys(payload, keys) || !payload.at("extents").is_array() ||
        !payload.at("free_size_known").is_boolean()) {
        throw std::invalid_argument("recovery point source volume fields are invalid");
    }
    contracts::RecoveryPointSourceVolume volume{
        unsigned_value<std::uint32_t>(payload, "volume_index"),
        payload.at("letter").get<std::string>(),
        payload.at("label").get<std::string>(),
        payload.at("filesystem").get<std::string>(),
        unsigned_value<std::uint64_t>(payload, "total_size_bytes"),
        unsigned_value<std::uint64_t>(payload, "free_size_bytes"),
        payload.at("free_size_known").get<bool>(),
        {}};
    if (volume.free_size_bytes > volume.total_size_bytes) {
        volume.free_size_bytes = volume.total_size_bytes;
    }
    for (const auto& item : payload.at("extents")) {
        volume.extents.push_back(parse_recovery_point_source_extent(item));
    }
    return volume;
}

[[nodiscard]] Json encode_recovery_point_layout(const contracts::RecoveryPointLayout& layout) {
    Json disks = Json::array();
    for (const auto& disk : layout.disks) {
        disks.push_back(encode_recovery_point_source_disk(disk));
    }
    Json volumes = Json::array();
    for (const auto& volume : layout.volumes) {
        volumes.push_back(encode_recovery_point_source_volume(volume));
    }
    return Json{{"repository_connection_id", layout.repository_connection_id},
                {"recovery_point_id", layout.recovery_point_id},
                {"disks", std::move(disks)},
                {"volumes", std::move(volumes)}};
}

[[nodiscard]] contracts::RecoveryPointLayout parse_recovery_point_layout(const Json& payload) {
    constexpr std::array<std::string_view, 4> keys{"repository_connection_id", "recovery_point_id",
                                                   "disks", "volumes"};
    if (!exact_keys(payload, keys) || !payload.at("disks").is_array() ||
        !payload.at("volumes").is_array()) {
        throw std::invalid_argument("recovery point layout fields are invalid");
    }
    contracts::RecoveryPointLayout layout;
    layout.repository_connection_id = payload.at("repository_connection_id").get<std::string>();
    layout.recovery_point_id = payload.at("recovery_point_id").get<std::string>();
    for (const auto& item : payload.at("disks")) {
        layout.disks.push_back(parse_recovery_point_source_disk(item));
    }
    for (const auto& item : payload.at("volumes")) {
        layout.volumes.push_back(parse_recovery_point_source_volume(item));
    }
    return layout;
}

[[nodiscard]] Json encode_schedule_trigger(const contracts::ScheduleTrigger& trigger) {
    return Json{{"kind", static_cast<std::uint8_t>(trigger.kind)},
                {"local_minutes_of_day", trigger.local_minutes_of_day},
                {"weekday_mask", trigger.weekday_mask},
                {"day_of_month_mask", trigger.day_of_month_mask},
                {"timezone_id", trigger.timezone_id}};
}

[[nodiscard]] contracts::ScheduleTrigger parse_schedule_trigger(const Json& payload) {
    constexpr std::array<std::string_view, 5> keys{"kind", "local_minutes_of_day", "weekday_mask",
                                                   "day_of_month_mask", "timezone_id"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("schedule trigger fields are invalid");
    }
    contracts::ScheduleTrigger trigger;
    trigger.kind =
        static_cast<contracts::ScheduleTriggerKind>(unsigned_value<std::uint8_t>(payload, "kind"));
    const auto& minutes = payload.at("local_minutes_of_day");
    if (!minutes.is_array() || minutes.empty() ||
        minutes.size() > contracts::kMaximumLocalMinutesOfDay) {
        throw std::invalid_argument("schedule trigger times are invalid");
    }
    // Default-constructed trigger must not keep a seed minute (would become extra 00:00).
    trigger.local_minutes_of_day.clear();
    trigger.local_minutes_of_day.reserve(minutes.size());
    for (const auto& item : minutes) {
        if (!item.is_number_unsigned() && !item.is_number_integer()) {
            throw std::invalid_argument("schedule trigger times are invalid");
        }
        const auto value = item.get<std::uint64_t>();
        if (value >= 24ULL * 60ULL) {
            throw std::invalid_argument("schedule trigger times are invalid");
        }
        trigger.local_minutes_of_day.push_back(static_cast<std::uint16_t>(value));
    }
    std::sort(trigger.local_minutes_of_day.begin(), trigger.local_minutes_of_day.end());
    trigger.local_minutes_of_day.erase(
        std::unique(trigger.local_minutes_of_day.begin(), trigger.local_minutes_of_day.end()),
        trigger.local_minutes_of_day.end());
    if (trigger.local_minutes_of_day.empty()) {
        throw std::invalid_argument("schedule trigger times are invalid");
    }
    trigger.weekday_mask = unsigned_value<std::uint8_t>(payload, "weekday_mask");
    trigger.day_of_month_mask = unsigned_value<std::uint32_t>(payload, "day_of_month_mask");
    trigger.timezone_id = payload.at("timezone_id").get<std::string>();
    return trigger;
}

[[nodiscard]] Json encode_selection_summary(const contracts::FileSelectionSummary& summary) {
    return Json{{"selection_id", summary.selection_id},
                {"display_label", summary.display_label},
                {"entry_kind", static_cast<std::uint8_t>(summary.entry_kind)},
                {"recursion", static_cast<std::uint8_t>(summary.recursion)},
                {"display_chain", summary.display_chain}};
}

[[nodiscard]] contracts::FileSelectionSummary parse_selection_summary(const Json& payload) {
    constexpr std::array<std::string_view, 5> keys{"selection_id", "display_label", "entry_kind",
                                                   "recursion", "display_chain"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("file selection summary fields are invalid");
    }
    contracts::FileSelectionSummary summary;
    summary.selection_id = payload.at("selection_id").get<std::string>();
    summary.display_label = payload.at("display_label").get<std::string>();
    summary.entry_kind =
        static_cast<contracts::FileEntryKind>(unsigned_value<std::uint8_t>(payload, "entry_kind"));
    summary.recursion =
        static_cast<contracts::FileRecursion>(unsigned_value<std::uint8_t>(payload, "recursion"));
    summary.display_chain = payload.at("display_chain").get<std::vector<std::string>>();
    return summary;
}

[[nodiscard]] Json encode_schedule(const contracts::ScheduleSummary& summary) {
    Json selections = Json::array();
    for (const auto& item : summary.selection_summaries) {
        selections.push_back(encode_selection_summary(item));
    }
    return Json{{"schedule_id", summary.schedule_id},
                {"display_name", summary.display_name},
                {"enabled", summary.enabled},
                {"content_kind", static_cast<std::uint8_t>(summary.content_kind)},
                {"source_ids", summary.source_ids},
                {"selection_summaries", std::move(selections)},
                {"repository_connection_id", summary.repository_connection_id},
                {"backup_type", static_cast<std::uint8_t>(summary.backup_type)},
                {"trigger", encode_schedule_trigger(summary.trigger)},
                {"next_run_utc_ms", optional_uint64_json(summary.next_run_utc_ms)},
                {"exclude_page_and_hibernation_files", summary.exclude_page_and_hibernation_files},
                {"deduplication_enabled", summary.deduplication_enabled},
                {"encryption_enabled", summary.encryption_enabled}};
}

[[nodiscard]] contracts::ScheduleSummary parse_schedule(const Json& payload) {
    constexpr std::array<std::string_view, 13> keys{"schedule_id",
                                                    "display_name",
                                                    "enabled",
                                                    "content_kind",
                                                    "source_ids",
                                                    "selection_summaries",
                                                    "repository_connection_id",
                                                    "backup_type",
                                                    "trigger",
                                                    "next_run_utc_ms",
                                                    "exclude_page_and_hibernation_files",
                                                    "deduplication_enabled",
                                                    "encryption_enabled"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("schedule summary fields are invalid");
    }
    contracts::ScheduleSummary summary;
    summary.schedule_id = payload.at("schedule_id").get<std::string>();
    summary.display_name = payload.at("display_name").get<std::string>();
    summary.enabled = payload.at("enabled").get<bool>();
    summary.content_kind =
        static_cast<contracts::ContentKind>(unsigned_value<std::uint8_t>(payload, "content_kind"));
    summary.source_ids = payload.at("source_ids").get<std::vector<std::string>>();
    for (const auto& item : payload.at("selection_summaries")) {
        summary.selection_summaries.push_back(parse_selection_summary(item));
    }
    summary.repository_connection_id = payload.at("repository_connection_id").get<std::string>();
    summary.backup_type =
        static_cast<contracts::BackupType>(unsigned_value<std::uint8_t>(payload, "backup_type"));
    summary.trigger = parse_schedule_trigger(payload.at("trigger"));
    summary.next_run_utc_ms = optional_uint64(payload.at("next_run_utc_ms"));
    summary.exclude_page_and_hibernation_files =
        payload.at("exclude_page_and_hibernation_files").get<bool>();
    summary.deduplication_enabled = payload.at("deduplication_enabled").get<bool>();
    summary.encryption_enabled = payload.at("encryption_enabled").get<bool>();
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
                {"source_disk_number", summary.source_disk_number},
                {"disk_size_bytes", summary.disk_size_bytes},
                {"started_utc_ms", summary.started_utc_ms},
                {"message_code", summary.message_code}};
}

[[nodiscard]] contracts::MountSessionSummary parse_mount_session(const Json& payload) {
    constexpr std::array<std::string_view, 8> keys{
        "session_id",         "recovery_point_id", "state",          "mount_point",
        "source_disk_number", "disk_size_bytes",   "started_utc_ms", "message_code"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("mount session fields are invalid");
    }
    contracts::MountSessionSummary summary;
    summary.session_id = payload.at("session_id").get<std::string>();
    summary.recovery_point_id = payload.at("recovery_point_id").get<std::string>();
    summary.state =
        static_cast<contracts::MountSessionState>(unsigned_value<std::uint8_t>(payload, "state"));
    summary.mount_point = payload.at("mount_point").get<std::string>();
    summary.source_disk_number = unsigned_value<std::uint32_t>(payload, "source_disk_number");
    summary.disk_size_bytes = unsigned_value<std::uint64_t>(payload, "disk_size_bytes");
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
                {"repository_connection_id", preflight.repository_connection_id},
                {"recovery_point_id", preflight.recovery_point_id},
                {"target_source_id", preflight.target_source_id},
                {"logical_size_bytes", preflight.logical_size_bytes},
                {"target_capacity_bytes", preflight.target_capacity_bytes},
                {"chain_depth", preflight.chain_depth},
                {"expires_utc_ms", preflight.expires_utc_ms},
                {"restore_eligible", preflight.restore_eligible},
                {"message_code", preflight.message_code}};
}

[[nodiscard]] contracts::RestorePreflight parse_restore_preflight(const Json& payload) {
    constexpr std::array<std::string_view, 10> keys{
        "preflight_token",  "repository_connection_id", "recovery_point_id",
        "target_source_id", "logical_size_bytes",       "target_capacity_bytes",
        "chain_depth",      "expires_utc_ms",           "restore_eligible",
        "message_code"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("restore preflight fields are invalid");
    }
    return {payload.at("preflight_token").get<std::string>(),
            payload.at("repository_connection_id").get<std::string>(),
            payload.at("recovery_point_id").get<std::string>(),
            payload.at("target_source_id").get<std::string>(),
            unsigned_value<std::uint64_t>(payload, "logical_size_bytes"),
            unsigned_value<std::uint64_t>(payload, "target_capacity_bytes"),
            unsigned_value<std::uint32_t>(payload, "chain_depth"),
            unsigned_value<std::uint64_t>(payload, "expires_utc_ms"),
            payload.at("restore_eligible").get<bool>(),
            payload.at("message_code").get<std::string>()};
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
    constexpr std::array<std::string_view, 6> keys{"recovery_point_id",        "backup_type",
                                                   "parent_recovery_point_id", "structural_state",
                                                   "authentication_state",     "chain_state"};
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
        "repository_connection_id", "recovery_point_id", "layers",      "restore_eligible",
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
        "plan_token", "operation_id",  "repository_connection_id", "root_recovery_point_id",
        "targets",    "expires_utc_ms"};
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
                                           : Json(nullptr)},
                {"free_bytes", acknowledgement.free_bytes ? Json(*acknowledgement.free_bytes)
                                                          : Json(nullptr)}};
}

[[nodiscard]] contracts::CommandAcknowledgement parse_command_ack(const Json& payload) {
    constexpr std::array<std::string_view, 5> keys{"command_id", "disposition", "resource_id",
                                                   "event_subscription", "free_bytes"};
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
    if (!payload.at("free_bytes").is_null()) {
        acknowledgement.free_bytes = unsigned_value<std::uint64_t>(payload, "free_bytes");
    }
    return acknowledgement;
}

[[nodiscard]] Json encode_task_result(const contracts::TaskResult& result) {
    return Json{
        {"schema_version", result.schema_version},
        {"job_id", result.job_id},
        {"trace_id", result.trace_id},
        {"outcome", static_cast<std::uint8_t>(result.outcome)},
        {"error_code", static_cast<std::uint32_t>(result.error_code)},
        {"logical_bytes", result.logical_bytes},
        {"stored_bytes", result.stored_bytes},
        {"chunk_count", result.chunk_count},
        {"entry_count", result.entry_count},
        {"stream_count", result.stream_count},
        {"deduplicated_block_count", result.deduplicated_block_count},
        {"deduplicated_logical_bytes", result.deduplicated_logical_bytes},
        {"message_code", result.message_code},
        {"warning_codes", result.warning_codes},
        {"partial_restore",
         result.partial_restore ? encode_partial_restore(*result.partial_restore) : Json(nullptr)},
        {"requested_backup_type",
         result.requested_backup_type ? Json(*result.requested_backup_type) : Json(nullptr)},
        {"effective_backup_type",
         result.effective_backup_type ? Json(*result.effective_backup_type) : Json(nullptr)},
        {"effective_parent_uuid",
         result.effective_parent_uuid ? Json(*result.effective_parent_uuid) : Json(nullptr)},
        {"incremental_downgrade_reason",
         result.incremental_downgrade_reason
             ? Json(static_cast<std::uint8_t>(*result.incremental_downgrade_reason))
             : Json(nullptr)}};
}

[[nodiscard]] contracts::TaskResult parse_task_result(const Json& payload) {
    constexpr std::array<std::string_view, 19> keys{"schema_version",
                                                    "job_id",
                                                    "trace_id",
                                                    "outcome",
                                                    "error_code",
                                                    "logical_bytes",
                                                    "stored_bytes",
                                                    "chunk_count",
                                                    "entry_count",
                                                    "stream_count",
                                                    "deduplicated_block_count",
                                                    "deduplicated_logical_bytes",
                                                    "message_code",
                                                    "warning_codes",
                                                    "partial_restore",
                                                    "requested_backup_type",
                                                    "effective_backup_type",
                                                    "effective_parent_uuid",
                                                    "incremental_downgrade_reason"};
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
    result.entry_count = unsigned_value<std::uint64_t>(payload, "entry_count");
    result.stream_count = unsigned_value<std::uint64_t>(payload, "stream_count");
    result.deduplicated_block_count =
        unsigned_value<std::uint64_t>(payload, "deduplicated_block_count");
    result.deduplicated_logical_bytes =
        unsigned_value<std::uint64_t>(payload, "deduplicated_logical_bytes");
    result.message_code = payload.at("message_code").get<std::string>();
    result.warning_codes = payload.at("warning_codes").get<std::vector<std::string>>();
    if (!payload.at("partial_restore").is_null()) {
        result.partial_restore = parse_partial_restore(payload.at("partial_restore"));
    }
    if (!payload.at("requested_backup_type").is_null()) {
        result.requested_backup_type =
            unsigned_value<std::uint8_t>(payload, "requested_backup_type");
    }
    if (!payload.at("effective_backup_type").is_null()) {
        result.effective_backup_type =
            unsigned_value<std::uint8_t>(payload, "effective_backup_type");
    }
    if (!payload.at("effective_parent_uuid").is_null()) {
        result.effective_parent_uuid = payload.at("effective_parent_uuid").get<std::string>();
    }
    if (!payload.at("incremental_downgrade_reason").is_null()) {
        const auto reason = static_cast<contracts::IncrementalDowngradeReason>(
            unsigned_value<std::uint8_t>(payload, "incremental_downgrade_reason"));
        if (!contracts::is_known_incremental_downgrade_reason(reason) ||
            reason == contracts::IncrementalDowngradeReason::kNone) {
            throw std::invalid_argument("task result incremental downgrade reason is invalid");
        }
        result.incremental_downgrade_reason = reason;
    }
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
    case contracts::ServiceRequestKind::kGetRecoveryPointLayout:
        return encode_recovery_point_layout(
            std::get<contracts::RecoveryPointLayout>(response.payload));
    case contracts::ServiceRequestKind::kBrowseFileSources:
    case contracts::ServiceRequestKind::kListRepositoryDirectories:
        return encode_page(std::get<contracts::FileSourceNodePage>(response.payload),
                           [](const contracts::FileSourceNode& node) {
                               return Json{
                                   {"node_token", node.node_token},
                                   {"display_name", node.display_name},
                                   {"entry_kind", static_cast<std::uint8_t>(node.entry_kind)},
                                   {"selectability", static_cast<std::uint8_t>(node.selectability)},
                                   {"has_children", node.has_children},
                                   {"is_directory", node.is_directory},
                                   {"availability", static_cast<std::uint8_t>(node.availability)},
                                   {"message_code", optional_string_json(node.message_code)}};
                           });
    case contracts::ServiceRequestKind::kListRecoveryPointEntries:
        return [&]() {
            const auto& page = std::get<contracts::RecoveryPointEntryPage>(response.payload);
            Json items = Json::array();
            for (const auto& item : page.items) {
                items.push_back(Json{{"entry_id", item.entry_id},
                                     {"display_name", item.display_name},
                                     {"entry_kind", static_cast<std::uint8_t>(item.entry_kind)},
                                     {"logical_size_bytes", item.logical_size_bytes},
                                     {"has_children", item.has_children},
                                     {"message_code", optional_string_json(item.message_code)}});
            }
            return Json{
                {"repository_connection_id", optional_string_json(page.repository_connection_id)},
                {"recovery_point_id", page.recovery_point_id},
                {"parent_entry_id", page.parent_entry_id},
                {"index_generation", page.index_generation},
                {"items", std::move(items)},
                {"continuation_token", optional_string_json(page.continuation_token)}};
        }();
    case contracts::ServiceRequestKind::kPrepareFileRestore: {
        const auto& preflight = std::get<contracts::FileRestorePreflight>(response.payload);
        return Json{
            {"preflight_token", preflight.preflight_token},
            {"repository_connection_id", optional_string_json(preflight.repository_connection_id)},
            {"recovery_point_id", preflight.recovery_point_id},
            {"entry_count", preflight.entry_count},
            {"logical_size_bytes", preflight.logical_size_bytes},
            {"target_free_bytes", preflight.target_free_bytes},
            {"conflict_policy", static_cast<std::uint8_t>(preflight.conflict_policy)},
            {"expires_utc_ms", preflight.expires_utc_ms},
            {"restore_eligible", preflight.restore_eligible},
            {"message_code", preflight.message_code}};
    }
    case contracts::ServiceRequestKind::kGetServiceSettings: {
        const auto& settings = std::get<contracts::ServiceSettings>(response.payload);
        return Json{{"job_retention_months", settings.job_retention_months},
                    {"updated_utc_ms", settings.updated_utc_ms}};
    }
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
    case contracts::ServiceRequestKind::kGetRecoveryPointLayout:
        return parse_recovery_point_layout(payload);
    case contracts::ServiceRequestKind::kBrowseFileSources:
    case contracts::ServiceRequestKind::kListRepositoryDirectories:
        return parse_page<contracts::FileSourceNode>(payload, [](const Json& item) {
            constexpr std::array<std::string_view, 8> keys{
                "node_token",   "display_name", "entry_kind",   "selectability",
                "has_children", "is_directory", "availability", "message_code"};
            if (!exact_keys(item, keys)) {
                throw std::invalid_argument("file source node fields are invalid");
            }
            contracts::FileSourceNode node;
            node.node_token = item.at("node_token").get<std::string>();
            node.display_name = item.at("display_name").get<std::string>();
            node.entry_kind = static_cast<contracts::FileEntryKind>(
                unsigned_value<std::uint8_t>(item, "entry_kind"));
            node.selectability = static_cast<contracts::FileNodeSelectability>(
                unsigned_value<std::uint8_t>(item, "selectability"));
            node.has_children = item.at("has_children").get<bool>();
            node.is_directory = item.at("is_directory").get<bool>();
            node.availability = static_cast<contracts::SourceAvailability>(
                unsigned_value<std::uint8_t>(item, "availability"));
            node.message_code = optional_string(item.at("message_code"));
            return node;
        });
    case contracts::ServiceRequestKind::kListRecoveryPointEntries: {
        constexpr std::array<std::string_view, 6> keys{"repository_connection_id",
                                                       "recovery_point_id",
                                                       "parent_entry_id",
                                                       "index_generation",
                                                       "items",
                                                       "continuation_token"};
        if (!exact_keys(payload, keys) || !payload.at("items").is_array()) {
            throw std::invalid_argument("recovery point entry page fields are invalid");
        }
        contracts::RecoveryPointEntryPage page;
        page.repository_connection_id = optional_string(payload.at("repository_connection_id"));
        page.recovery_point_id = payload.at("recovery_point_id").get<std::string>();
        page.parent_entry_id = payload.at("parent_entry_id").get<std::string>();
        page.index_generation = payload.at("index_generation").get<std::string>();
        page.continuation_token = optional_string(payload.at("continuation_token"));
        for (const auto& item : payload.at("items")) {
            constexpr std::array<std::string_view, 6> item_keys{
                "entry_id",           "display_name", "entry_kind",
                "logical_size_bytes", "has_children", "message_code"};
            if (!exact_keys(item, item_keys)) {
                throw std::invalid_argument("recovery point entry fields are invalid");
            }
            contracts::RecoveryPointEntrySummary summary;
            summary.entry_id = item.at("entry_id").get<std::string>();
            summary.display_name = item.at("display_name").get<std::string>();
            summary.entry_kind = static_cast<contracts::FileEntryKind>(
                unsigned_value<std::uint8_t>(item, "entry_kind"));
            summary.logical_size_bytes = unsigned_value<std::uint64_t>(item, "logical_size_bytes");
            summary.has_children = item.at("has_children").get<bool>();
            summary.message_code = optional_string(item.at("message_code"));
            page.items.push_back(std::move(summary));
        }
        return page;
    }
    case contracts::ServiceRequestKind::kPrepareFileRestore: {
        constexpr std::array<std::string_view, 10> keys{
            "preflight_token", "repository_connection_id", "recovery_point_id",
            "entry_count",     "logical_size_bytes",       "target_free_bytes",
            "conflict_policy", "expires_utc_ms",           "restore_eligible",
            "message_code"};
        if (!exact_keys(payload, keys)) {
            throw std::invalid_argument("file restore preflight fields are invalid");
        }
        contracts::FileRestorePreflight preflight;
        preflight.preflight_token = payload.at("preflight_token").get<std::string>();
        preflight.repository_connection_id =
            optional_string(payload.at("repository_connection_id"));
        preflight.recovery_point_id = payload.at("recovery_point_id").get<std::string>();
        preflight.entry_count = unsigned_value<std::uint64_t>(payload, "entry_count");
        preflight.logical_size_bytes = unsigned_value<std::uint64_t>(payload, "logical_size_bytes");
        preflight.target_free_bytes = unsigned_value<std::uint64_t>(payload, "target_free_bytes");
        preflight.conflict_policy = static_cast<contracts::FileConflictPolicy>(
            unsigned_value<std::uint8_t>(payload, "conflict_policy"));
        preflight.expires_utc_ms = unsigned_value<std::uint64_t>(payload, "expires_utc_ms");
        preflight.restore_eligible = payload.at("restore_eligible").get<bool>();
        preflight.message_code = payload.at("message_code").get<std::string>();
        return preflight;
    }
    case contracts::ServiceRequestKind::kGetServiceSettings: {
        constexpr std::array<std::string_view, 2> keys{"job_retention_months", "updated_utc_ms"};
        if (!exact_keys(payload, keys)) {
            throw std::invalid_argument("service settings fields are invalid");
        }
        contracts::ServiceSettings settings;
        settings.job_retention_months =
            unsigned_value<std::uint8_t>(payload, "job_retention_months");
        settings.updated_utc_ms = unsigned_value<std::uint64_t>(payload, "updated_utc_ms");
        return settings;
    }
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
