#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aegra::format {

inline constexpr std::uint32_t kManifestSchemaVersion = 1;
inline constexpr std::uint8_t kManifestContentKindVolumeSet = 1;
inline constexpr std::uint8_t kManifestContentKindFileSet = 2;
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
    std::uint8_t fingerprint_algorithm{contracts::kSelectionFingerprintAlgorithmSha256V1};
    std::array<std::byte, contracts::kSelectionFingerprintBytes> selection_fingerprint{};
    /// Sorted unique by volume_identity; required on Full/Incremental after successful backup.
    std::vector<contracts::FileJournalCheckpoint> journal_checkpoints;
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
    std::vector<ProviderExtension> extensions;
};

[[nodiscard]] base::Result<void> validate_manifest(const Manifest& manifest);

} // namespace aegra::format
