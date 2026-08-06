#include "aegra/personal_repository/catalog.h"

#include "json_codec.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace aegra::personal_repository {
namespace {

[[nodiscard]] base::Result<void> valid() { return base::Result<void>::success(); }

[[nodiscard]] base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure(detail::invalid(message));
}

[[nodiscard]] bool known_backup_type(const format::BackupType type) noexcept {
    switch (type) {
    case format::BackupType::kFull:
    case format::BackupType::kIncremental:
    case format::BackupType::kDifferential:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_parent(const CatalogEntry& entry) noexcept {
    if (entry.backup_type == format::BackupType::kFull) {
        return !entry.parent_uuid.has_value();
    }
    return entry.parent_uuid.has_value() && detail::is_canonical_uuid(entry.parent_uuid.value()) &&
           entry.parent_uuid.value() != entry.file_uuid;
}

[[nodiscard]] bool valid_descriptor_prefixes(const RepositoryDescriptor& descriptor) noexcept {
    return descriptor.archive_prefix == "archives" &&
           descriptor.catalog_prefix == "catalog/recovery-points" &&
           descriptor.deletion_prefix == "catalog/deletions" &&
           descriptor.staging_prefix == "staging";
}

[[nodiscard]] bool parse_part_index(const std::string& key, const std::string& main_key,
                                    std::uint32_t& part_index) noexcept {
    if (key.size() != main_key.size() + 4 || key[main_key.size()] != '.') {
        return false;
    }
    part_index = 0;
    for (std::size_t index = main_key.size() + 1; index < key.size(); ++index) {
        const auto value = static_cast<unsigned char>(key[index]);
        if (std::isdigit(value) == 0) {
            return false;
        }
        part_index = part_index * 10 + static_cast<std::uint32_t>(value - '0');
    }
    return part_index != 0;
}

[[nodiscard]] bool valid_member_sequence(const DeletionTarget& target) noexcept {
    if (target.members.empty() || target.members.back().key != target.archive_main_key) {
        return false;
    }
    std::size_t index = 0;
    if (target.members.front().key == target.archive_main_key + ".bhx") {
        ++index;
    }
    std::uint32_t previous_part = 0;
    bool has_previous_part = false;
    for (; index + 1 < target.members.size(); ++index) {
        std::uint32_t current_part = 0;
        if (!parse_part_index(target.members[index].key, target.archive_main_key, current_part) ||
            (has_previous_part && current_part + 1 != previous_part)) {
            return false;
        }
        previous_part = current_part;
        has_previous_part = true;
    }
    std::set<std::string, std::less<>> unique;
    for (const auto& member : target.members) {
        unique.insert(member.key);
    }
    return unique.size() == target.members.size() && (!has_previous_part || previous_part == 1);
}

} // namespace

base::Result<void> validate_repository_descriptor(const RepositoryDescriptor& descriptor) {
    if (descriptor.schema_version != kRepositorySchemaVersion ||
        descriptor.layout_version != kRepositoryLayoutVersion ||
        descriptor.kind != "aegra_personal_repository") {
        return invalid("repository descriptor version or kind is invalid");
    }
    if (!detail::is_canonical_uuid(descriptor.repository_uuid) ||
        !valid_descriptor_prefixes(descriptor)) {
        return invalid("repository descriptor identity or prefixes are invalid");
    }
    return valid();
}

base::Result<void> validate_catalog_entry(const CatalogEntry& entry) {
    if (entry.schema_version != kCatalogSchemaVersion ||
        entry.kind != "aegra_personal_recovery_point" ||
        entry.format_version != kPersonalArchiveFormatVersion ||
        entry.structural_state != "complete") {
        return invalid("catalog entry version, kind, or state is invalid");
    }
    if (!detail::is_canonical_uuid(entry.repository_uuid) ||
        !detail::is_canonical_uuid(entry.file_uuid) ||
        !detail::is_canonical_uuid(entry.backup_set_uuid) ||
        !known_backup_type(entry.backup_type)) {
        return invalid("catalog entry identity is invalid");
    }
    if (!valid_parent(entry) ||
        !detail::is_archive_main_key(entry.archive_main_key, entry.file_uuid) ||
        entry.archive_main_key.size() > kMaximumRepositoryKeyBytes || entry.split_part_count == 0 ||
        entry.split_part_count > kMaximumSplitPartCount || entry.catalog_generation == 0) {
        return invalid("catalog entry chain or location is invalid");
    }
    if (entry.source_volume_ids.size() != entry.source_count) {
        return invalid("catalog entry source volume identity count is invalid");
    }
    std::set<std::string, std::less<>> unique_sources;
    for (const auto& volume_id : entry.source_volume_ids) {
        if (volume_id.empty() || volume_id.size() > kMaximumRepositoryKeyBytes ||
            !unique_sources.insert(volume_id).second) {
            return invalid("catalog entry source volume identity is invalid");
        }
    }
    return valid();
}

base::Result<void> validate_deletion_tombstone(const DeletionTombstone& tombstone,
                                               const CatalogCodecLimits& limits) {
    if (tombstone.schema_version != kDeletionSchemaVersion ||
        tombstone.kind != "aegra_personal_deletion" ||
        !detail::is_canonical_uuid(tombstone.repository_uuid) ||
        !detail::is_canonical_uuid(tombstone.operation_uuid)) {
        return invalid("deletion tombstone identity is invalid");
    }
    if (tombstone.targets.empty() || tombstone.targets.size() > limits.maximum_deletion_targets) {
        return invalid("deletion tombstone target count is invalid");
    }
    std::set<std::string, std::less<>> target_uuids;
    for (const auto& target : tombstone.targets) {
        if (!detail::is_canonical_uuid(target.file_uuid) || target.catalog_generation == 0 ||
            !detail::is_archive_main_key(target.archive_main_key, target.file_uuid) ||
            target.archive_main_key.size() > kMaximumRepositoryKeyBytes ||
            std::ranges::any_of(target.members,
                                [](const DeletionMember& member) {
                                    return member.key.size() > kMaximumRepositoryKeyBytes ||
                                           (member.generation && (member.generation->empty() ||
                                                                  member.generation->size() >
                                                                      kMaximumRepositoryKeyBytes));
                                }) ||
            target.members.size() > limits.maximum_archive_members ||
            !valid_member_sequence(target) || !target_uuids.insert(target.file_uuid).second) {
            return invalid("deletion tombstone target is invalid");
        }
    }
    return valid();
}

} // namespace aegra::personal_repository
