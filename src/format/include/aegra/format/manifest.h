#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/boot_check.h"
#include "aegra/contracts/file_set.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aegra::format {

inline constexpr std::uint32_t kManifestSchemaVersion = 2;
inline constexpr std::uint8_t kManifestContentKindVolumeSet = 1;
inline constexpr std::uint8_t kManifestContentKindFileSet = 2;
inline constexpr std::uint16_t kBootProfileVersion = 1;
inline constexpr std::uint16_t kBootProbeProtocolVersion =
    contracts::kBootCheckProbeProtocolVersion;
inline constexpr std::uint8_t kBootLayoutFingerprintAlgorithmSha256V1 = 1;
inline constexpr std::size_t kBootLayoutFingerprintBytes = 32;
using Guid = std::array<std::byte, 16>;

enum class PartitionStyle : std::uint8_t {
    kMbr = 0,
    kGpt = 1,
    kRaw = 2,
};

enum class ConsistencyLevel : std::uint8_t {
    kCrash = 0,
    kFilesystem = 1,
    kApplication = 2,
};

enum class BackupType : std::uint8_t {
    kFull = 1,
    kIncremental = 2,
    kDifferential = 3,
};

enum class BootFirmwareMode : std::uint8_t {
    kBios = 1,
    kUefi = 2,
};

enum class BootOsArchitecture : std::uint8_t {
    kX64 = 1,
};

enum class BootSecurityState : std::uint8_t {
    kUnknown = 0,
    kDisabled = 1,
    kEnabled = 2,
};

enum class BootHardwareState : std::uint8_t {
    kUnknown = 0,
    kAbsent = 1,
    kPresent = 2,
};

struct Partition final {
    std::uint32_t partition_number{0};
    std::uint64_t offset{0};
    std::uint64_t size{0};
    PartitionStyle style{PartitionStyle::kRaw};
    bool is_active{false};
    /// MBR partition type (0 when GPT/RAW). Used for reserved-partition filtering.
    std::uint8_t mbr_type{0};
    /// GPT type GUID as lowercase "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", empty when MBR/RAW.
    std::string gpt_type_guid;
    /// GPT partition name (UTF-8), empty when unavailable.
    std::string gpt_name;
    std::string volume_label;
    std::string filesystem;
    std::string volume_guid;
};

struct RawDiskLayout final {
    std::vector<std::byte> mbr_sector;
    std::vector<std::byte> gpt_primary_header;
    std::vector<std::byte> gpt_partition_entries;
    std::vector<std::byte> gpt_backup_header;
    std::vector<std::byte> gpt_backup_entries;
};

struct Disk final {
    std::uint32_t disk_number{0};
    std::uint64_t disk_size{0};
    std::uint32_t bytes_per_sector{0};
    std::uint64_t total_sectors{0};
    PartitionStyle partition_style{PartitionStyle::kRaw};
    std::string model;
    std::string serial;
    std::string media_type;
    std::vector<Partition> partitions;
    RawDiskLayout raw_layout;
};

struct VolumeExtent final {
    std::uint32_t disk_number{0};
    std::uint32_t partition_number{0};
    std::uint64_t physical_offset{0};
    std::uint64_t volume_offset{0};
    std::uint64_t length{0};
    std::string extent_role{"basic"};
};

struct Volume final {
    std::uint32_t volume_index{0};
    std::string volume_id;
    std::string volume_guid;
    std::vector<std::string> mount_points;
    std::string filesystem;
    std::string label;
    std::uint64_t total_size{0};
    /// Free bytes at backup when `free_size_known` (GetDiskFreeSpaceEx); else 0.
    std::uint64_t free_size{0};
    /// False when volume free-space metadata could not be queried; UI must not draw used ratio.
    bool free_size_known{false};
    std::uint32_t cluster_size{0};
    bool vss_required{false};
    bool vss_used{false};
    ConsistencyLevel consistency_level{ConsistencyLevel::kCrash};
    std::vector<VolumeExtent> extents;
};

struct SystemInfo final {
    std::string hostname;
    std::string machine_guid;
    std::string os_name;
    std::string os_version;
    std::string os_architecture;
    std::string collection_time_utc;
};

struct BackupJob final {
    BackupType backup_type{BackupType::kFull};
    std::string created_utc;
    std::string application_version;
    std::string description;
};

/// Encrypted metadata baseline for file_set Full and Incremental (FI1).
struct FileSetBaseline final {
    std::uint8_t fingerprint_algorithm{0};
    std::array<std::byte, contracts::kSelectionFingerprintBytes> selection_fingerprint{};
    contracts::FileChangeDetectionMethod change_detection_method{
        contracts::FileChangeDetectionMethod::kNone};
};

/// Authenticated facts needed to decide whether a volume_set recovery point can be boot-checked.
/// The profile is absent for data-only or incomplete system-disk selections.
struct BootProfile final {
    std::uint16_t profile_version{kBootProfileVersion};
    std::uint32_t system_disk_number{0};
    std::uint32_t windows_volume_index{0};
    std::vector<std::uint32_t> required_boot_partition_numbers;
    BootFirmwareMode firmware_mode{BootFirmwareMode::kBios};
    BootOsArchitecture os_architecture{BootOsArchitecture::kX64};
    std::string os_build;
    BootSecurityState secure_boot_state{BootSecurityState::kUnknown};
    BootHardwareState tpm_state{BootHardwareState::kUnknown};
    BootSecurityState bitlocker_state{BootSecurityState::kUnknown};
    std::uint32_t logical_sector_size{0};
    std::uint8_t layout_fingerprint_algorithm{kBootLayoutFingerprintAlgorithmSha256V1};
    std::array<std::byte, kBootLayoutFingerprintBytes> layout_fingerprint{};
    std::uint16_t probe_protocol_version{kBootProbeProtocolVersion};
    std::string aegra_service_version;
};

struct ProviderExtension final {
    std::string key;
    std::vector<std::byte> payload;
};

struct Manifest final {
    std::uint32_t schema_version{kManifestSchemaVersion};
    std::uint8_t content_kind{kManifestContentKindVolumeSet};
    std::vector<Disk> disks;
    SystemInfo system;
    BackupJob backup_job;
    std::vector<Volume> volumes;
    /// Present when content_kind=file_set; empty/default invalid for file_set validation.
    FileSetBaseline file_set_baseline;
    /// Present only when a volume_set contains a complete supported Windows system disk.
    std::optional<BootProfile> boot_profile;
    std::vector<ProviderExtension> extensions;
};

[[nodiscard]] base::Result<void> validate_manifest(const Manifest& manifest);

/// Stable binary preimage for BootProfile.layout_fingerprint (SHA-256 algorithm id 1).
[[nodiscard]] base::Result<std::vector<std::byte>>
encode_boot_disk_layout_fingerprint_preimage(const Disk& disk);

/// Identity/layout compatibility required between adjacent incremental volume_set layers.
[[nodiscard]] bool compatible_boot_profiles(const std::optional<BootProfile>& left,
                                            const std::optional<BootProfile>& right) noexcept;

} // namespace aegra::format
