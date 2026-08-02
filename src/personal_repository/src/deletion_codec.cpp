#include "aegra/personal_repository/catalog.h"

#include "json_codec.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::personal_repository {
namespace {

using detail::Json;

constexpr std::array<std::string_view, 6> kTombstoneKeys = {
    "schema_version", "kind", "repository_uuid", "operation_uuid", "created_utc_ms", "targets"};
constexpr std::array<std::string_view, 4> kTargetKeys = {"file_uuid", "catalog_generation",
                                                         "archive_main_key", "member_keys"};

[[nodiscard]] base::Result<DeletionTarget> parse_target(const Json& value) {
    if (!value.is_object()) {
        return base::Result<DeletionTarget>::failure(
            detail::corrupt("deletion target is not an object"));
    }
    auto keys = detail::require_exact_keys(value, kTargetKeys);
    if (!keys) {
        return base::Result<DeletionTarget>::failure(keys.error());
    }
    auto generation = detail::get_unsigned<std::uint64_t>(value, "catalog_generation");
    if (!generation) {
        return base::Result<DeletionTarget>::failure(generation.error());
    }
    try {
        DeletionTarget result;
        result.file_uuid = value.at("file_uuid").get<std::string>();
        result.catalog_generation = generation.value();
        result.archive_main_key = value.at("archive_main_key").get<std::string>();
        result.member_keys = value.at("member_keys").get<std::vector<std::string>>();
        return base::Result<DeletionTarget>::success(std::move(result));
    } catch (const Json::exception&) {
        return base::Result<DeletionTarget>::failure(
            detail::corrupt("deletion target field has invalid type"));
    }
}

[[nodiscard]] base::Result<std::vector<DeletionTarget>>
parse_targets(const Json& value, const CatalogCodecLimits& limits) {
    if (!value.is_array() || value.empty() || value.size() > limits.maximum_deletion_targets) {
        return base::Result<std::vector<DeletionTarget>>::failure(
            detail::corrupt("deletion target count is invalid"));
    }
    std::vector<DeletionTarget> result;
    result.reserve(value.size());
    for (const auto& item : value) {
        auto target = parse_target(item);
        if (!target) {
            return base::Result<std::vector<DeletionTarget>>::failure(target.error());
        }
        result.push_back(std::move(target).value());
    }
    return base::Result<std::vector<DeletionTarget>>::success(std::move(result));
}

[[nodiscard]] base::Result<DeletionTombstone> parse_tombstone(const Json& root,
                                                              const CatalogCodecLimits& limits) {
    auto keys = detail::require_exact_keys(root, kTombstoneKeys);
    if (!keys) {
        return base::Result<DeletionTombstone>::failure(keys.error());
    }
    auto targets = parse_targets(root.at("targets"), limits);
    if (!targets) {
        return base::Result<DeletionTombstone>::failure(targets.error());
    }
    auto schema = detail::get_unsigned<std::uint32_t>(root, "schema_version");
    auto created = detail::get_unsigned<std::uint64_t>(root, "created_utc_ms");
    if (!schema || !created) {
        return base::Result<DeletionTombstone>::failure(!schema ? schema.error() : created.error());
    }
    try {
        DeletionTombstone result;
        result.schema_version = schema.value();
        result.kind = root.at("kind").get<std::string>();
        result.repository_uuid = root.at("repository_uuid").get<std::string>();
        result.operation_uuid = root.at("operation_uuid").get<std::string>();
        result.created_utc_ms = created.value();
        result.targets = std::move(targets).value();
        return base::Result<DeletionTombstone>::success(std::move(result));
    } catch (const Json::exception&) {
        return base::Result<DeletionTombstone>::failure(
            detail::corrupt("deletion tombstone field has invalid type"));
    }
}

[[nodiscard]] Json encode_target(const DeletionTarget& target) {
    return {{"file_uuid", target.file_uuid},
            {"catalog_generation", target.catalog_generation},
            {"archive_main_key", target.archive_main_key},
            {"member_keys", target.member_keys}};
}

} // namespace

base::Result<std::string> encode_deletion_tombstone_json(const DeletionTombstone& tombstone,
                                                         const CatalogCodecLimits& limits) {
    auto valid = validate_deletion_tombstone(tombstone, limits);
    if (!valid) {
        return base::Result<std::string>::failure(valid.error());
    }
    Json targets = Json::array();
    for (const auto& target : tombstone.targets) {
        targets.push_back(encode_target(target));
    }
    const Json root = {{"schema_version", tombstone.schema_version},
                       {"kind", tombstone.kind},
                       {"repository_uuid", tombstone.repository_uuid},
                       {"operation_uuid", tombstone.operation_uuid},
                       {"created_utc_ms", tombstone.created_utc_ms},
                       {"targets", std::move(targets)}};
    return base::Result<std::string>::success(root.dump());
}

base::Result<DeletionTombstone> decode_deletion_tombstone_json(const std::string_view encoded,
                                                               const CatalogCodecLimits& limits) {
    auto root = detail::parse_json_object(encoded, limits);
    if (!root) {
        return base::Result<DeletionTombstone>::failure(root.error());
    }
    auto result = parse_tombstone(root.value(), limits);
    if (!result) {
        return result;
    }
    auto valid = validate_deletion_tombstone(result.value(), limits);
    return valid ? result
                 : base::Result<DeletionTombstone>::failure(
                       detail::corrupt("deletion tombstone validation failed"));
}

} // namespace aegra::personal_repository
