#pragma once

#include "aegra/base/result.h"
#include "aegra/format/manifest.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::personal_repository {

inline constexpr std::uint32_t kRepositorySchemaVersion = 1;
inline constexpr std::uint32_t kRepositoryLayoutVersion = 1;
inline constexpr std::uint32_t kCatalogSchemaVersion = 2;
inline constexpr std::uint32_t kDeletionSchemaVersion = 1;
inline constexpr std::uint32_t kPersonalArchiveFormatVersion = 7;
inline constexpr std::uint32_t kMaximumSplitPartCount = 1'000;
inline constexpr std::uint32_t kMaximumRepositoryKeyBytes = 1'024;

inline constexpr std::string_view kCatalogContentKindVolumeSet = "volume_set";
inline constexpr std::string_view kCatalogContentKindFileSet = "file_set";

struct RepositoryDescriptor final {
    std::uint32_t schema_version{kRepositorySchemaVersion};
    std::string kind{"aegra_personal_repository"};
    std::string repository_uuid;
    std::uint64_t created_utc_ms{0};
    std::string archive_prefix{"archives"};
    std::string catalog_prefix{"catalog/recovery-points"};
    std::string deletion_prefix{"catalog/deletions"};
    std::string staging_prefix{"staging"};
    std::uint32_t layout_version{kRepositoryLayoutVersion};

    bool operator==(const RepositoryDescriptor&) const = default;
};

struct CatalogEntry final {
    std::uint32_t schema_version{kCatalogSchemaVersion};
    std::string kind{"aegra_personal_recovery_point"};
    std::string repository_uuid;
    std::string file_uuid;
    std::string backup_set_uuid;
    std::optional<std::string> parent_uuid;
    format::BackupType backup_type{format::BackupType::kFull};
    std::string content_kind{std::string(kCatalogContentKindVolumeSet)};
    std::string archive_main_key;
    std::uint32_t split_part_count{1};
    bool has_sidecar{false};
    std::uint32_t format_version{kPersonalArchiveFormatVersion};
    std::uint64_t created_utc_ms{0};
    std::uint64_t logical_size_bytes{0};
    std::uint64_t stored_size_bytes{0};
    std::uint32_t source_count{0};
    /// Ordered stable volume identities (canonical Volume GUID paths) matching the archive
    /// source order. Used for incremental parent selection without opening the parent archive.
    /// Must be empty for file_set.
    std::vector<std::string> source_volume_ids;
    std::uint64_t file_entry_count{0};
    std::uint64_t file_stream_count{0};
    std::string structural_state{"complete"};
    std::uint64_t catalog_generation{1};
    /// file_set only: lowercase hex of 32-byte selection fingerprint; empty when unknown.
    std::string file_selection_fingerprint;
    /// file_set only: true when authenticated baseline (fingerprint + checkpoints) is available.
    bool file_baseline_available{false};

    bool operator==(const CatalogEntry&) const = default;
};

struct DeletionMember final {
    std::string key;
    std::optional<std::string> generation;

    bool operator==(const DeletionMember&) const = default;
};

struct DeletionTarget final {
    std::string file_uuid;
    std::uint64_t catalog_generation{0};
    std::string content_kind{std::string(kCatalogContentKindVolumeSet)};
    std::string archive_main_key;
    std::vector<DeletionMember> members;

    bool operator==(const DeletionTarget&) const = default;
};

struct DeletionTombstone final {
    std::uint32_t schema_version{kDeletionSchemaVersion};
    std::string kind{"aegra_personal_deletion"};
    std::string repository_uuid;
    std::string operation_uuid;
    std::uint64_t created_utc_ms{0};
    std::vector<DeletionTarget> targets;

    bool operator==(const DeletionTombstone&) const = default;
};

struct CatalogCodecLimits final {
    std::uint64_t maximum_document_bytes{4ULL * 1024ULL * 1024ULL};
    std::uint32_t maximum_deletion_targets{10'000};
    std::uint32_t maximum_archive_members{1'001};
};

[[nodiscard]] base::Result<void>
validate_repository_descriptor(const RepositoryDescriptor& descriptor);
[[nodiscard]] base::Result<void> validate_catalog_entry(const CatalogEntry& entry);
[[nodiscard]] base::Result<void> validate_deletion_tombstone(const DeletionTombstone& tombstone,
                                                             const CatalogCodecLimits& limits = {});

[[nodiscard]] base::Result<std::string>
encode_repository_descriptor_json(const RepositoryDescriptor& descriptor);
[[nodiscard]] base::Result<RepositoryDescriptor>
decode_repository_descriptor_json(std::string_view encoded, const CatalogCodecLimits& limits = {});

[[nodiscard]] base::Result<std::string> encode_catalog_entry_json(const CatalogEntry& entry);
[[nodiscard]] base::Result<CatalogEntry>
decode_catalog_entry_json(std::string_view encoded, const CatalogCodecLimits& limits = {});

[[nodiscard]] base::Result<std::string>
encode_deletion_tombstone_json(const DeletionTombstone& tombstone,
                               const CatalogCodecLimits& limits = {});
[[nodiscard]] base::Result<DeletionTombstone>
decode_deletion_tombstone_json(std::string_view encoded, const CatalogCodecLimits& limits = {});

} // namespace aegra::personal_repository
