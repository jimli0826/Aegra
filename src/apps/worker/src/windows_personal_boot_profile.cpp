#include "windows_personal_boot_profile.h"

#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/adapters/windows_disk/windows_disk.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::worker::detail {
namespace {

constexpr std::string_view kEfiSystemPartitionGuid =
    "c12a7328-f81f-11d2-ba4b-00a0c93ec93b";

[[nodiscard]] bool same_path(const std::filesystem::path& left,
                             const std::filesystem::path& right) noexcept {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

[[nodiscard]] std::optional<std::uint32_t>
selected_volume_index(const std::vector<PreparedVolumeMetadata>& sources,
                      const std::filesystem::path& path) {
    for (std::size_t index = 0; index < sources.size(); ++index) {
        if (same_path(sources[index].volume_guid_path, path)) {
            return static_cast<std::uint32_t>(index);
        }
    }
    return std::nullopt;
}

[[nodiscard]] const format::Disk* find_disk(const format::Manifest& manifest,
                                            const std::uint32_t disk_number) noexcept {
    const auto found =
        std::ranges::find(manifest.disks, disk_number, &format::Disk::disk_number);
    return found == manifest.disks.end() ? nullptr : &*found;
}

[[nodiscard]] bool selected_sources_cover_system_disk(
    const std::vector<PreparedVolumeMetadata>& sources,
    const std::vector<adapters::windows_disk::WindowsVolumeInfo>& live_volumes,
    const std::uint32_t system_disk_number) {
    for (const auto& volume : live_volumes) {
        const bool touches_system_disk = std::ranges::any_of(
            volume.extents, [=](const auto& extent) { return extent.disk_number == system_disk_number; });
        if (!touches_system_disk) {
            continue;
        }
        if (!volume.volume_size_available || volume.total_size_bytes == 0 ||
            !volume.disk_extents_available || volume.extents.size() != 1 ||
            volume.extents.front().disk_number != system_disk_number ||
            !selected_volume_index(sources, volume.volume_guid_path)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool selected_partition(const format::Manifest& manifest,
                                      const std::uint32_t disk_number,
                                      const std::uint32_t partition_number) noexcept {
    return std::ranges::any_of(manifest.volumes, [&](const format::Volume& volume) {
        return std::ranges::any_of(volume.extents, [&](const format::VolumeExtent& extent) {
            return extent.disk_number == disk_number &&
                   extent.partition_number == partition_number;
        });
    });
}

[[nodiscard]] bool raw_layout_supports_boot_check(
    const format::Disk& disk,
    const adapters::windows_disk::WindowsFirmwareMode firmware) noexcept {
    if (disk.raw_layout.mbr_sector.size() < 512U ||
        disk.raw_layout.mbr_sector[510] != std::byte{0x55} ||
        disk.raw_layout.mbr_sector[511] != std::byte{0xAA}) {
        return false;
    }
    if (firmware == adapters::windows_disk::WindowsFirmwareMode::kUefi) {
        return !disk.raw_layout.gpt_primary_header.empty() &&
               !disk.raw_layout.gpt_partition_entries.empty() &&
               !disk.raw_layout.gpt_backup_header.empty() &&
               !disk.raw_layout.gpt_backup_entries.empty();
    }
    const auto bootstrap_end = disk.raw_layout.mbr_sector.begin() + 440;
    return std::ranges::any_of(disk.raw_layout.mbr_sector.begin(), bootstrap_end,
                               [](const std::byte item) { return item != std::byte{0}; });
}

[[nodiscard]] std::optional<std::vector<std::uint32_t>> required_boot_partitions(
    const format::Manifest& manifest, const format::Disk& disk,
    const adapters::windows_disk::WindowsBootEnvironment& environment,
    const std::uint32_t windows_partition_number) {
    std::set<std::uint32_t> required{windows_partition_number};
    if (environment.firmware_mode == adapters::windows_disk::WindowsFirmwareMode::kUefi) {
        const auto efi_count = std::ranges::count(
            disk.partitions, kEfiSystemPartitionGuid, &format::Partition::gpt_type_guid);
        if (disk.partition_style != format::PartitionStyle::kGpt || efi_count != 1) {
            return std::nullopt;
        }
        const auto efi = std::ranges::find(disk.partitions, kEfiSystemPartitionGuid,
                                           &format::Partition::gpt_type_guid);
        if (efi == disk.partitions.end() ||
            !selected_partition(manifest, disk.disk_number, efi->partition_number)) {
            return std::nullopt;
        }
        required.insert(efi->partition_number);
    } else {
        const auto active_count =
            std::ranges::count(disk.partitions, true, &format::Partition::is_active);
        if (disk.partition_style != format::PartitionStyle::kMbr || active_count != 1) {
            return std::nullopt;
        }
        const auto active = std::ranges::find(disk.partitions, true, &format::Partition::is_active);
        if (active == disk.partitions.end() ||
            !selected_partition(manifest, disk.disk_number, active->partition_number)) {
            return std::nullopt;
        }
        required.insert(active->partition_number);
    }
    return std::vector<std::uint32_t>(required.begin(), required.end());
}

[[nodiscard]] format::BootSecurityState map_security_state(
    const adapters::windows_disk::WindowsSecurityState state) noexcept {
    switch (state) {
    case adapters::windows_disk::WindowsSecurityState::kDisabled:
        return format::BootSecurityState::kDisabled;
    case adapters::windows_disk::WindowsSecurityState::kEnabled:
        return format::BootSecurityState::kEnabled;
    default:
        return format::BootSecurityState::kUnknown;
    }
}

[[nodiscard]] format::BootHardwareState map_hardware_state(
    const adapters::windows_disk::WindowsHardwareState state) noexcept {
    switch (state) {
    case adapters::windows_disk::WindowsHardwareState::kAbsent:
        return format::BootHardwareState::kAbsent;
    case adapters::windows_disk::WindowsHardwareState::kPresent:
        return format::BootHardwareState::kPresent;
    default:
        return format::BootHardwareState::kUnknown;
    }
}

} // namespace

base::Result<void>
collect_windows_boot_profile(const std::string_view application_version,
                             const std::vector<PreparedVolumeMetadata>& sources,
                             format::Manifest& manifest) {
    manifest.boot_profile.reset();
    auto environment = adapters::windows_disk::inspect_windows_boot_environment();
    if (!environment) {
        return base::Result<void>::success();
    }
    manifest.system.os_name = "Windows";
    manifest.system.os_version = environment.value().os_build;
    manifest.system.os_architecture =
        environment.value().os_architecture == adapters::windows_disk::WindowsOsArchitecture::kX64
            ? "x64"
            : "unsupported";

    const auto windows_index =
        selected_volume_index(sources, environment.value().windows_volume_guid_path);
    if (!windows_index || *windows_index >= manifest.volumes.size() ||
        environment.value().os_architecture !=
            adapters::windows_disk::WindowsOsArchitecture::kX64 ||
        application_version.empty()) {
        return base::Result<void>::success();
    }
    const auto& windows_volume = manifest.volumes[*windows_index];
    if (windows_volume.extents.size() != 1) {
        return base::Result<void>::success();
    }
    const auto& windows_extent = windows_volume.extents.front();
    const auto* disk = find_disk(manifest, windows_extent.disk_number);
    if (disk == nullptr || disk->bytes_per_sector != 512U ||
        !raw_layout_supports_boot_check(*disk, environment.value().firmware_mode)) {
        return base::Result<void>::success();
    }

    auto live_volumes = adapters::windows_disk::WindowsVolumeEnumerator::enumerate();
    if (!live_volumes ||
        !selected_sources_cover_system_disk(sources, live_volumes.value(), disk->disk_number)) {
        return base::Result<void>::success();
    }
    auto required = required_boot_partitions(manifest, *disk, environment.value(),
                                             windows_extent.partition_number);
    if (!required) {
        return base::Result<void>::success();
    }

    auto preimage = format::encode_boot_disk_layout_fingerprint_preimage(*disk);
    if (!preimage) {
        return base::Result<void>::failure(preimage.error());
    }
    auto fingerprint = adapters::crypto_sodium::sha256(preimage.value());
    if (!fingerprint) {
        return base::Result<void>::failure(fingerprint.error());
    }

    format::BootProfile profile;
    profile.system_disk_number = disk->disk_number;
    profile.windows_volume_index = *windows_index;
    profile.required_boot_partition_numbers = std::move(*required);
    profile.firmware_mode =
        environment.value().firmware_mode == adapters::windows_disk::WindowsFirmwareMode::kUefi
            ? format::BootFirmwareMode::kUefi
            : format::BootFirmwareMode::kBios;
    profile.os_build = environment.value().os_build;
    profile.secure_boot_state = map_security_state(environment.value().secure_boot_state);
    profile.tpm_state = map_hardware_state(environment.value().tpm_state);
    profile.bitlocker_state = map_security_state(environment.value().bitlocker_state);
    profile.logical_sector_size = disk->bytes_per_sector;
    profile.layout_fingerprint = fingerprint.value();
    profile.aegra_service_version = application_version;
    manifest.boot_profile = std::move(profile);
    return base::Result<void>::success();
}

} // namespace aegra::apps::worker::detail
