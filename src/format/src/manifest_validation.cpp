#include "aegra/format/manifest.h"

#include "aegra/base/error.h"

#include <set>
#include <string>
#include <utility>

namespace aegra::format {
namespace {

[[nodiscard]] base::Result<void> invalid(std::string message) {
    return base::Result<void>::failure({base::ErrorCode::kInvalidArgument, std::move(message)});
}

[[nodiscard]] base::Result<void> validate_partitions(const Disk& disk) {
    std::set<std::uint32_t> numbers;
    for (const auto& partition : disk.partitions) {
        if (partition.size == 0) {
            return invalid("manifest partition size must be non-zero");
        }
        if (!numbers.insert(partition.partition_number).second) {
            return invalid("manifest contains duplicate partition numbers on one disk");
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_disks(const std::vector<Disk>& disks) {
    std::set<std::uint32_t> numbers;
    for (const auto& disk : disks) {
        if (disk.disk_size == 0 || disk.bytes_per_sector == 0) {
            return invalid("manifest disk size and sector size must be non-zero");
        }
        if (!numbers.insert(disk.disk_number).second) {
            return invalid("manifest contains duplicate disk numbers");
        }
        auto partitions = validate_partitions(disk);
        if (!partitions) {
            return partitions;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_extent(const VolumeExtent& extent,
                                                 const std::set<std::uint32_t>& disk_numbers) {
    if (extent.length == 0) {
        return invalid("manifest volume extent length must be non-zero");
    }
    if (!disk_numbers.contains(extent.disk_number)) {
        return invalid("manifest volume extent references an unknown disk");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
validate_volume_extents(const Volume& volume, const std::set<std::uint32_t>& disk_numbers) {
    for (const auto& extent : volume.extents) {
        auto result = validate_extent(extent, disk_numbers);
        if (!result) {
            return result;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_volume_identity(const Volume& volume,
                                                          std::set<std::uint32_t>& volume_indices,
                                                          std::set<std::string>& volume_ids) {
    if (volume.volume_id.empty() || volume.total_size == 0) {
        return invalid("manifest volume id and size must be present");
    }
    if (!volume_indices.insert(volume.volume_index).second) {
        return invalid("manifest contains a duplicate volume index");
    }
    if (!volume_ids.insert(volume.volume_id).second) {
        return invalid("manifest contains a duplicate volume id");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_volumes(const Manifest& manifest) {
    std::set<std::uint32_t> disk_numbers;
    std::set<std::uint32_t> volume_indices;
    std::set<std::string> volume_ids;
    for (const auto& disk : manifest.disks) {
        disk_numbers.insert(disk.disk_number);
    }
    for (const auto& volume : manifest.volumes) {
        auto identity = validate_volume_identity(volume, volume_indices, volume_ids);
        if (!identity) {
            return identity;
        }
        auto extents = validate_volume_extents(volume, disk_numbers);
        if (!extents) {
            return extents;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_extensions(const Manifest& manifest) {
    std::set<std::string> keys;
    for (const auto& extension : manifest.extensions) {
        if (extension.key.empty() || !keys.insert(extension.key).second) {
            return invalid("manifest extension keys must be non-empty and unique");
        }
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<void> validate_manifest(const Manifest& manifest) {
    if (manifest.schema_version != kManifestSchemaVersion) {
        return invalid("manifest schema version is unsupported");
    }
    if (manifest.content_kind != kManifestContentKindVolumeSet &&
        manifest.content_kind != kManifestContentKindFileSet) {
        return invalid("manifest content kind is invalid");
    }
    if (manifest.backup_job.created_utc.empty()) {
        return invalid("manifest backup creation time is required");
    }
    if (manifest.content_kind == kManifestContentKindFileSet) {
        if (!manifest.disks.empty() || !manifest.volumes.empty()) {
            return invalid("file_set manifest must not include disks or volumes");
        }
        if (manifest.backup_job.backup_type != BackupType::kFull) {
            return invalid("file_set manifest requires full backup type");
        }
        return validate_extensions(manifest);
    }
    if (manifest.volumes.empty()) {
        return invalid("volume_set manifest requires at least one volume");
    }
    auto disks = validate_disks(manifest.disks);
    if (!disks) {
        return disks;
    }
    auto volumes = validate_volumes(manifest);
    if (!volumes) {
        return volumes;
    }
    return validate_extensions(manifest);
}

} // namespace aegra::format
