#include "service_protocol_json.h"

#include "aegra/contracts/repository_query.h"
#include "aegra/contracts/service_control.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// RecoveryPointSummary / RecoveryPointPage / ServiceRecoveryPointPage codec (protocol V4 §4.9).
// Split from service_protocol_response_json.cpp to keep that unit within the size limit.
namespace aegra::apps::service::protocol_json {
namespace {

[[nodiscard]] Json
encode_recovery_point_check(const std::optional<contracts::RecoveryPointCheckStatus>& check) {
    if (!check) {
        return Json(nullptr);
    }
    return Json{{"state", static_cast<std::uint8_t>(check->state)},
                {"message_code", check->message_code},
                {"job_id", check->job_id},
                {"completed_utc_ms", check->completed_utc_ms}};
}

[[nodiscard]] std::optional<contracts::RecoveryPointCheckStatus>
parse_recovery_point_check(const Json& payload) {
    if (payload.is_null()) {
        return std::nullopt;
    }
    constexpr std::array<std::string_view, 4> keys{"state", "message_code", "job_id",
                                                   "completed_utc_ms"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("recovery point check fields are invalid");
    }
    contracts::RecoveryPointCheckStatus check;
    check.state = static_cast<contracts::RecoveryPointCheckState>(
        unsigned_value<std::uint8_t>(payload, "state"));
    check.message_code = payload.at("message_code").get<std::string>();
    check.job_id = payload.at("job_id").get<std::string>();
    check.completed_utc_ms = unsigned_value<std::uint64_t>(payload, "completed_utc_ms");
    return check;
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
                {"has_sidecar", point.has_sidecar},
                {"verify_check", encode_recovery_point_check(point.verify_check)},
                {"boot_check", encode_recovery_point_check(point.boot_check)}};
}
[[nodiscard]] contracts::RecoveryPointSummary parse_recovery_point(const Json& payload) {
    constexpr std::array<std::string_view, 15> keys{"file_uuid",
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
                                                    "has_sidecar",
                                                    "verify_check",
                                                    "boot_check"};
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
    point.verify_check = parse_recovery_point_check(payload.at("verify_check"));
    point.boot_check = parse_recovery_point_check(payload.at("boot_check"));
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

} // namespace

Json encode_service_recovery_point_page(const contracts::ServiceRecoveryPointPage& page) {
    return Json{{"repository_connection_id", optional_string_json(page.repository_connection_id)},
                {"catalog", encode_recovery_point_page(page.catalog)}};
}

contracts::ServiceRecoveryPointPage parse_service_recovery_point_page(const Json& payload) {
    constexpr std::array<std::string_view, 2> keys{"repository_connection_id", "catalog"};
    if (!exact_keys(payload, keys)) {
        throw std::invalid_argument("service recovery point page fields are invalid");
    }
    return {optional_string(payload.at("repository_connection_id")),
            parse_recovery_point_page(payload.at("catalog"))};
}

} // namespace aegra::apps::service::protocol_json
