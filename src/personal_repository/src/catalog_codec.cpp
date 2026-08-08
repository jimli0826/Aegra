#include "aegra/personal_repository/catalog.h"

#include "json_codec.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::personal_repository {
namespace {

using detail::Json;

constexpr std::array<std::string_view, 9> kDescriptorKeys = {
    "schema_version",  "kind",           "repository_uuid",
    "created_utc_ms",  "archive_prefix", "catalog_prefix",
    "deletion_prefix", "staging_prefix", "layout_version"};
constexpr std::array<std::string_view, 21> kCatalogKeys = {
    "schema_version",     "kind",
    "repository_uuid",    "file_uuid",
    "backup_set_uuid",    "parent_uuid",
    "backup_type",        "content_kind",
    "archive_main_key",   "split_part_count",
    "has_sidecar",        "format_version",
    "created_utc_ms",     "logical_size_bytes",
    "stored_size_bytes",  "source_count",
    "source_volume_ids",  "file_entry_count",
    "file_stream_count",  "structural_state",
    "catalog_generation",
};

[[nodiscard]] base::Result<std::optional<std::string>> parse_parent_uuid(const Json& value) {
    if (value.is_null()) {
        return base::Result<std::optional<std::string>>::success(std::nullopt);
    }
    if (!value.is_string()) {
        return base::Result<std::optional<std::string>>::failure(
            detail::corrupt("catalog parent UUID has invalid type"));
    }
    return base::Result<std::optional<std::string>>::success(value.get<std::string>());
}

[[nodiscard]] base::Result<RepositoryDescriptor> parse_descriptor(const Json& root) {
    auto keys = detail::require_exact_keys(root, kDescriptorKeys);
    if (!keys) {
        return base::Result<RepositoryDescriptor>::failure(keys.error());
    }
    auto schema = detail::get_unsigned<std::uint32_t>(root, "schema_version");
    auto created = detail::get_unsigned<std::uint64_t>(root, "created_utc_ms");
    auto layout = detail::get_unsigned<std::uint32_t>(root, "layout_version");
    if (!schema || !created || !layout) {
        return base::Result<RepositoryDescriptor>::failure(
            !schema ? schema.error() : (!created ? created.error() : layout.error()));
    }
    try {
        RepositoryDescriptor result;
        result.schema_version = schema.value();
        result.kind = root.at("kind").get<std::string>();
        result.repository_uuid = root.at("repository_uuid").get<std::string>();
        result.created_utc_ms = created.value();
        result.archive_prefix = root.at("archive_prefix").get<std::string>();
        result.catalog_prefix = root.at("catalog_prefix").get<std::string>();
        result.deletion_prefix = root.at("deletion_prefix").get<std::string>();
        result.staging_prefix = root.at("staging_prefix").get<std::string>();
        result.layout_version = layout.value();
        return base::Result<RepositoryDescriptor>::success(std::move(result));
    } catch (const Json::exception&) {
        return base::Result<RepositoryDescriptor>::failure(
            detail::corrupt("repository descriptor field has invalid type"));
    }
}

[[nodiscard]] base::Result<CatalogEntry> parse_catalog(const Json& root) {
    auto keys = detail::require_exact_keys(root, kCatalogKeys);
    if (!keys) {
        return base::Result<CatalogEntry>::failure(keys.error());
    }
    auto parent = parse_parent_uuid(root.at("parent_uuid"));
    auto type = detail::parse_backup_type(root.at("backup_type"));
    if (!parent || !type) {
        return base::Result<CatalogEntry>::failure(!parent ? parent.error() : type.error());
    }
    auto schema = detail::get_unsigned<std::uint32_t>(root, "schema_version");
    auto part_count = detail::get_unsigned<std::uint32_t>(root, "split_part_count");
    auto format_version = detail::get_unsigned<std::uint32_t>(root, "format_version");
    auto created = detail::get_unsigned<std::uint64_t>(root, "created_utc_ms");
    auto logical = detail::get_unsigned<std::uint64_t>(root, "logical_size_bytes");
    auto stored = detail::get_unsigned<std::uint64_t>(root, "stored_size_bytes");
    auto source_count = detail::get_unsigned<std::uint32_t>(root, "source_count");
    auto file_entry_count = detail::get_unsigned<std::uint64_t>(root, "file_entry_count");
    auto file_stream_count = detail::get_unsigned<std::uint64_t>(root, "file_stream_count");
    auto generation = detail::get_unsigned<std::uint64_t>(root, "catalog_generation");
    if (!schema || !part_count || !format_version || !created || !logical || !stored ||
        !source_count || !file_entry_count || !file_stream_count || !generation) {
        return base::Result<CatalogEntry>::failure(
            detail::corrupt("catalog entry unsigned field is invalid"));
    }
    try {
        CatalogEntry result;
        result.schema_version = schema.value();
        result.kind = root.at("kind").get<std::string>();
        result.repository_uuid = root.at("repository_uuid").get<std::string>();
        result.file_uuid = root.at("file_uuid").get<std::string>();
        result.backup_set_uuid = root.at("backup_set_uuid").get<std::string>();
        result.parent_uuid = std::move(parent).value();
        result.backup_type = type.value();
        result.content_kind = root.at("content_kind").get<std::string>();
        result.archive_main_key = root.at("archive_main_key").get<std::string>();
        result.split_part_count = part_count.value();
        result.has_sidecar = root.at("has_sidecar").get<bool>();
        result.format_version = format_version.value();
        result.created_utc_ms = created.value();
        result.logical_size_bytes = logical.value();
        result.stored_size_bytes = stored.value();
        result.source_count = source_count.value();
        result.source_volume_ids = root.at("source_volume_ids").get<std::vector<std::string>>();
        result.file_entry_count = file_entry_count.value();
        result.file_stream_count = file_stream_count.value();
        result.structural_state = root.at("structural_state").get<std::string>();
        result.catalog_generation = generation.value();
        return base::Result<CatalogEntry>::success(std::move(result));
    } catch (const Json::exception&) {
        return base::Result<CatalogEntry>::failure(
            detail::corrupt("catalog entry field has invalid type"));
    }
}

} // namespace

base::Result<std::string>
encode_repository_descriptor_json(const RepositoryDescriptor& descriptor) {
    auto valid = validate_repository_descriptor(descriptor);
    if (!valid) {
        return base::Result<std::string>::failure(valid.error());
    }
    const Json root = {{"schema_version", descriptor.schema_version},
                       {"kind", descriptor.kind},
                       {"repository_uuid", descriptor.repository_uuid},
                       {"created_utc_ms", descriptor.created_utc_ms},
                       {"archive_prefix", descriptor.archive_prefix},
                       {"catalog_prefix", descriptor.catalog_prefix},
                       {"deletion_prefix", descriptor.deletion_prefix},
                       {"staging_prefix", descriptor.staging_prefix},
                       {"layout_version", descriptor.layout_version}};
    return base::Result<std::string>::success(root.dump());
}

base::Result<RepositoryDescriptor>
decode_repository_descriptor_json(const std::string_view encoded,
                                  const CatalogCodecLimits& limits) {
    auto root = detail::parse_json_object(encoded, limits);
    if (!root) {
        return base::Result<RepositoryDescriptor>::failure(root.error());
    }
    auto result = parse_descriptor(root.value());
    if (!result) {
        return result;
    }
    auto valid = validate_repository_descriptor(result.value());
    return valid ? result
                 : base::Result<RepositoryDescriptor>::failure(
                       detail::corrupt("repository descriptor validation failed"));
}

base::Result<std::string> encode_catalog_entry_json(const CatalogEntry& entry) {
    auto valid = validate_catalog_entry(entry);
    if (!valid) {
        return base::Result<std::string>::failure(valid.error());
    }
    const Json root = {
        {"schema_version", entry.schema_version},
        {"kind", entry.kind},
        {"repository_uuid", entry.repository_uuid},
        {"file_uuid", entry.file_uuid},
        {"backup_set_uuid", entry.backup_set_uuid},
        {"parent_uuid", entry.parent_uuid ? Json(*entry.parent_uuid) : Json(nullptr)},
        {"backup_type", detail::backup_type_name(entry.backup_type)},
        {"content_kind", entry.content_kind},
        {"archive_main_key", entry.archive_main_key},
        {"split_part_count", entry.split_part_count},
        {"has_sidecar", entry.has_sidecar},
        {"format_version", entry.format_version},
        {"created_utc_ms", entry.created_utc_ms},
        {"logical_size_bytes", entry.logical_size_bytes},
        {"stored_size_bytes", entry.stored_size_bytes},
        {"source_count", entry.source_count},
        {"source_volume_ids", entry.source_volume_ids},
        {"file_entry_count", entry.file_entry_count},
        {"file_stream_count", entry.file_stream_count},
        {"structural_state", entry.structural_state},
        {"catalog_generation", entry.catalog_generation},
    };
    return base::Result<std::string>::success(root.dump());
}

base::Result<CatalogEntry> decode_catalog_entry_json(const std::string_view encoded,
                                                     const CatalogCodecLimits& limits) {
    auto root = detail::parse_json_object(encoded, limits);
    if (!root) {
        return base::Result<CatalogEntry>::failure(root.error());
    }
    auto result = parse_catalog(root.value());
    if (!result) {
        return result;
    }
    auto valid = validate_catalog_entry(result.value());
    return valid ? result
                 : base::Result<CatalogEntry>::failure(
                       detail::corrupt("catalog entry validation failed"));
}

} // namespace aegra::personal_repository
