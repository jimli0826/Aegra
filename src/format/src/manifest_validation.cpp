#include "aegra/format/manifest.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::format {
namespace {

constexpr std::string_view kEfiSystemPartitionGuid =
    "c12a7328-f81f-11d2-ba4b-00a0c93ec93b";

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

[[nodiscard]] bool known_security_state(const BootSecurityState state) noexcept {
    return state == BootSecurityState::kUnknown || state == BootSecurityState::kDisabled ||
           state == BootSecurityState::kEnabled;
}

[[nodiscard]] bool known_hardware_state(const BootHardwareState state) noexcept {
    return state == BootHardwareState::kUnknown || state == BootHardwareState::kAbsent ||
           state == BootHardwareState::kPresent;
}

[[nodiscard]] bool raw_layout_complete(const Disk& disk) noexcept {
    if (disk.raw_layout.mbr_sector.size() < 512U ||
        disk.raw_layout.mbr_sector[510] != std::byte{0x55} ||
        disk.raw_layout.mbr_sector[511] != std::byte{0xAA}) {
        return false;
    }
    if (disk.partition_style == PartitionStyle::kMbr) {
        return true;
    }
    return disk.partition_style == PartitionStyle::kGpt &&
           !disk.raw_layout.gpt_primary_header.empty() &&
           !disk.raw_layout.gpt_partition_entries.empty() &&
           !disk.raw_layout.gpt_backup_header.empty() &&
           !disk.raw_layout.gpt_backup_entries.empty();
}

[[nodiscard]] base::Result<void> validate_boot_profile_values(const BootProfile& profile) {
    if (profile.profile_version != kBootProfileVersion ||
        profile.probe_protocol_version != kBootProbeProtocolVersion) {
        return invalid("manifest boot_profile version is unsupported");
    }
    if (profile.firmware_mode != BootFirmwareMode::kBios &&
        profile.firmware_mode != BootFirmwareMode::kUefi) {
        return invalid("manifest boot_profile firmware mode is invalid");
    }
    if (profile.os_architecture != BootOsArchitecture::kX64 || profile.os_build.empty() ||
        profile.aegra_service_version.empty()) {
        return invalid("manifest boot_profile guest identity is invalid");
    }
    if (!known_security_state(profile.secure_boot_state) ||
        !known_hardware_state(profile.tpm_state) ||
        !known_security_state(profile.bitlocker_state)) {
        return invalid("manifest boot_profile security state is invalid");
    }
    if (profile.logical_sector_size != 512U ||
        profile.layout_fingerprint_algorithm != kBootLayoutFingerprintAlgorithmSha256V1 ||
        std::ranges::all_of(profile.layout_fingerprint,
                            [](const std::byte item) { return item == std::byte{0}; })) {
        return invalid("manifest boot_profile disk compatibility is invalid");
    }
    if (profile.required_boot_partition_numbers.empty() ||
        !std::ranges::is_sorted(profile.required_boot_partition_numbers) ||
        std::ranges::adjacent_find(profile.required_boot_partition_numbers) !=
            profile.required_boot_partition_numbers.end()) {
        return invalid("manifest boot_profile partition list is invalid");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_boot_firmware_layout(const BootProfile& profile,
                                                               const Disk& disk) {
    if (profile.firmware_mode == BootFirmwareMode::kUefi &&
        disk.partition_style != PartitionStyle::kGpt) {
        return invalid("manifest UEFI boot_profile requires GPT");
    }
    if (profile.firmware_mode == BootFirmwareMode::kBios &&
        disk.partition_style != PartitionStyle::kMbr) {
        return invalid("manifest BIOS boot_profile requires MBR");
    }
    if (profile.firmware_mode == BootFirmwareMode::kBios &&
        !std::ranges::any_of(disk.raw_layout.mbr_sector.begin(),
                             disk.raw_layout.mbr_sector.begin() + 440,
                             [](const std::byte item) { return item != std::byte{0}; })) {
        return invalid("manifest BIOS boot_profile has no bootstrap code");
    }
    if (profile.firmware_mode == BootFirmwareMode::kUefi) {
        const auto efi_count =
            std::ranges::count(disk.partitions, kEfiSystemPartitionGuid,
                               &Partition::gpt_type_guid);
        const auto efi = std::ranges::find(disk.partitions, kEfiSystemPartitionGuid,
                                           &Partition::gpt_type_guid);
        if (efi_count != 1 ||
            !std::ranges::binary_search(profile.required_boot_partition_numbers,
                                        efi->partition_number)) {
            return invalid("manifest UEFI boot_profile omits the unique ESP");
        }
    } else {
        const auto active_count =
            std::ranges::count(disk.partitions, true, &Partition::is_active);
        const auto active = std::ranges::find(disk.partitions, true, &Partition::is_active);
        if (active_count != 1 ||
            !std::ranges::binary_search(profile.required_boot_partition_numbers,
                                        active->partition_number)) {
            return invalid("manifest BIOS boot_profile omits the unique active partition");
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_required_boot_partitions(
    const Manifest& manifest, const BootProfile& profile, const Disk& disk,
    const Volume& windows_volume) {
    for (const auto number : profile.required_boot_partition_numbers) {
        const auto partition = std::ranges::find(disk.partitions, number,
                                                 &Partition::partition_number);
        if (partition == disk.partitions.end()) {
            return invalid("manifest boot_profile references an unknown partition");
        }
        const bool backed_up = std::ranges::any_of(manifest.volumes, [&](const Volume& volume) {
            return std::ranges::any_of(volume.extents, [&](const VolumeExtent& extent) {
                return extent.disk_number == profile.system_disk_number &&
                       extent.partition_number == number;
            });
        });
        if (!backed_up) {
            return invalid("manifest boot_profile partition is not backed up");
        }
    }
    const auto windows_partition = windows_volume.extents.front().partition_number;
    if (!std::ranges::binary_search(profile.required_boot_partition_numbers, windows_partition)) {
        return invalid("manifest boot_profile omits the Windows partition");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_boot_profile(const Manifest& manifest) {
    if (!manifest.boot_profile) {
        return base::Result<void>::success();
    }
    const auto& profile = *manifest.boot_profile;
    auto values = validate_boot_profile_values(profile);
    if (!values) {
        return values;
    }
    const auto disk = std::ranges::find(manifest.disks, profile.system_disk_number,
                                        &Disk::disk_number);
    const auto windows_volume = std::ranges::find(manifest.volumes, profile.windows_volume_index,
                                                  &Volume::volume_index);
    if (disk == manifest.disks.end() || windows_volume == manifest.volumes.end() ||
        disk->bytes_per_sector != profile.logical_sector_size || !raw_layout_complete(*disk) ||
        windows_volume->extents.size() != 1 ||
        windows_volume->extents.front().disk_number != profile.system_disk_number) {
        return invalid("manifest boot_profile references an incompatible system disk");
    }
    auto firmware = validate_boot_firmware_layout(profile, *disk);
    if (!firmware) {
        return firmware;
    }
    return validate_required_boot_partitions(manifest, profile, *disk, *windows_volume);
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
        if (!manifest.disks.empty() || !manifest.volumes.empty() || manifest.boot_profile) {
            return invalid("file_set manifest must not include disks, volumes, or boot_profile");
        }
        if (manifest.backup_job.backup_type != BackupType::kFull &&
            manifest.backup_job.backup_type != BackupType::kIncremental) {
            return invalid("file_set manifest backup_type must be full or incremental");
        }
        contracts::FileSelectionFingerprint fingerprint;
        fingerprint.algorithm_id = manifest.file_set_baseline.fingerprint_algorithm;
        fingerprint.digest = manifest.file_set_baseline.selection_fingerprint;
        auto fp = contracts::validate_file_selection_fingerprint(fingerprint);
        if (!fp) {
            return invalid("file_set manifest selection_fingerprint is invalid");
        }
        if (!contracts::is_known_file_change_detection_method(
                manifest.file_set_baseline.change_detection_method) ||
            manifest.file_set_baseline.change_detection_method !=
                contracts::FileChangeDetectionMethod::kMtimeSizeV1) {
            return invalid("file_set manifest change_detection_method is invalid");
        }
        return validate_extensions(manifest);
    }
    if (manifest.file_set_baseline.fingerprint_algorithm != 0 ||
        !std::all_of(manifest.file_set_baseline.selection_fingerprint.begin(),
                     manifest.file_set_baseline.selection_fingerprint.end(),
                     [](const std::byte item) { return item == std::byte{0}; }) ||
        manifest.file_set_baseline.change_detection_method !=
            contracts::FileChangeDetectionMethod::kNone) {
        return invalid("volume_set manifest cannot carry file_set_baseline");
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
    auto boot_profile = validate_boot_profile(manifest);
    if (!boot_profile) {
        return boot_profile;
    }
    return validate_extensions(manifest);
}

} // namespace aegra::format
