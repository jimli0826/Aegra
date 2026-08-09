#pragma once

#include "aegra/ports/block_io.h"
#include "aegra/ports/source_inventory.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_disk {

enum class WindowsBlockSourceKind {
    kStableFile,
    kVssSnapshot,
    kRawVolume,
};

struct WindowsBlockSourceOpenRequest final {
    std::filesystem::path path;
    WindowsBlockSourceKind kind{WindowsBlockSourceKind::kStableFile};
    std::optional<std::uint64_t> expected_size_bytes;
};

class WindowsBlockSource final : public ports::IBlockSource {
  public:
    ~WindowsBlockSource() override;
    WindowsBlockSource(const WindowsBlockSource&) = delete;
    WindowsBlockSource& operator=(const WindowsBlockSource&) = delete;
    WindowsBlockSource(WindowsBlockSource&&) = delete;
    WindowsBlockSource& operator=(WindowsBlockSource&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<WindowsBlockSource>>
    open(const WindowsBlockSourceOpenRequest& request);
    [[nodiscard]] static bool
    is_vss_snapshot_device_path(const std::filesystem::path& path) noexcept;
    [[nodiscard]] static bool
    is_canonical_volume_guid_path(const std::filesystem::path& path) noexcept;

    [[nodiscard]] std::uint64_t size_bytes() const noexcept override;
    [[nodiscard]] base::Result<std::size_t> read(std::uint64_t offset,
                                                 std::span<std::byte> destination,
                                                 base::CancellationToken cancellation) override;

  private:
    struct Impl;

    explicit WindowsBlockSource(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

enum class WindowsBlockSinkKind {
    kStableFile,
    kVolume,
    /// `\\.\PhysicalDrive{N}` whole-disk restore target (non-system only).
    kPhysicalDisk,
};

struct WindowsBlockSinkOpenRequest final {
    std::filesystem::path path;
    WindowsBlockSinkKind kind{WindowsBlockSinkKind::kStableFile};
    std::optional<std::uint64_t> expected_capacity_bytes;
    /// Archive paths that must not live on the target volume/disk (required for volume/disk).
    std::vector<std::filesystem::path> protected_sources;
    /// Expected source disk size for capacity preflight (physical disk only).
    std::optional<std::uint64_t> minimum_capacity_bytes;
    /// Expected sector size from backup metadata (physical disk only; 0 = skip check).
    std::uint32_t expected_bytes_per_sector{0};
};

class WindowsBlockSink final : public ports::IBlockSink {
  public:
    ~WindowsBlockSink() override;
    WindowsBlockSink(const WindowsBlockSink&) = delete;
    WindowsBlockSink& operator=(const WindowsBlockSink&) = delete;
    WindowsBlockSink(WindowsBlockSink&&) = delete;
    WindowsBlockSink& operator=(WindowsBlockSink&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<WindowsBlockSink>>
    open(const WindowsBlockSinkOpenRequest& request);
    [[nodiscard]] static bool
    is_canonical_volume_guid_path(const std::filesystem::path& path) noexcept;
    [[nodiscard]] static bool
    is_physical_drive_path(const std::filesystem::path& path) noexcept;
    /// Parses N from `\\.\PhysicalDriveN` (case-insensitive). Returns nullopt on failure.
    [[nodiscard]] static std::optional<std::uint32_t>
    physical_drive_number(const std::filesystem::path& path) noexcept;

    [[nodiscard]] std::uint64_t capacity_bytes() const noexcept override;
    [[nodiscard]] base::Result<void> write(std::uint64_t offset, std::span<const std::byte> source,
                                           base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> flush(base::CancellationToken cancellation) override;

  private:
    struct Impl;

    explicit WindowsBlockSink(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

struct WindowsVolumeExtent final {
    std::uint32_t disk_number{0};
    std::uint64_t disk_offset_bytes{0};
    std::uint64_t length_bytes{0};
};

struct WindowsVolumeInfo final {
    std::filesystem::path volume_guid_path;
    std::vector<std::filesystem::path> mount_points;
    std::string label;
    std::string filesystem;
    std::uint64_t total_size_bytes{0};
    std::uint64_t free_size_bytes{0};
    std::uint32_t cluster_size_bytes{0};
    std::vector<WindowsVolumeExtent> extents;
    bool filesystem_metadata_available{false};
    bool volume_size_available{false};
    bool disk_extents_available{false};
    bool is_read_only{false};
};

[[nodiscard]] bool supports_vss_snapshot(const WindowsVolumeInfo& volume) noexcept;

/// Free-cluster skip plan built from FSCTL_GET_VOLUME_BITMAP (old BackupEngine free-skip).
/// free_ranges are sorted, non-overlapping [start, end) byte ranges that are classified as FREE
/// without reading the underlying device.
struct FreeSkipPlan final {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> free_ranges;
    std::uint64_t free_bytes{0};
    std::uint64_t total_bytes{0};
    std::uint64_t protected_prefix_bytes{0};
    std::string filesystem;
    bool applied{false};
};

/// Builds a free-skip plan for a volume or VSS snapshot device path.
/// On unsupported filesystem or bitmap failure, returns applied=false (caller backs up all bytes).
[[nodiscard]] FreeSkipPlan build_free_skip_plan(const std::filesystem::path& device_path,
                                                std::string_view filesystem,
                                                std::uint64_t total_size_bytes,
                                                std::uint32_t cluster_size_bytes);

/// Adds pagefile.sys / hiberfil.sys / swapfile.sys extent ranges into the free-skip plan
/// (AipCopy ExcludeJunkFiles / AddFileToExcludedClusters).
///
/// `read_device_path` must be the **same** device namespace used for block reads:
/// - raw live volume: canonical Volume GUID path;
/// - VSS: snapshot device object (`\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN`).
/// Extents are resolved on that root (FSCTL_GET_RETRIEVAL_POINTERS, then MFT fallback).
/// Never pass a live Volume GUID when the plan applies to a snapshot device.
/// Returns total excluded bytes newly covered (best-effort).
[[nodiscard]] std::uint64_t
merge_page_and_hibernation_exclusions(FreeSkipPlan& plan,
                                      const std::filesystem::path& read_device_path,
                                      std::uint32_t cluster_size_bytes);

/// Shrinks FREE ranges inward to archive block boundaries. A final range may end at the logical
/// volume size when the last archive block is partial. Mixed DATA/FREE archive blocks remain DATA.
void align_free_skip_plan(FreeSkipPlan& plan, std::uint32_t archive_block_size_bytes);

/// IBlockSource that reports FREE extents and forwards DATA reads to an inner source.
class FreeSkipBlockSource final : public ports::IBlockSource {
  public:
    ~FreeSkipBlockSource() override;
    FreeSkipBlockSource(const FreeSkipBlockSource&) = delete;
    FreeSkipBlockSource& operator=(const FreeSkipBlockSource&) = delete;
    FreeSkipBlockSource(FreeSkipBlockSource&&) = delete;
    FreeSkipBlockSource& operator=(FreeSkipBlockSource&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<FreeSkipBlockSource>>
    wrap(std::unique_ptr<ports::IBlockSource> inner, FreeSkipPlan plan);

    [[nodiscard]] std::uint64_t size_bytes() const noexcept override;
    [[nodiscard]] base::Result<ports::BlockExtent>
    describe_extent(std::uint64_t logical_offset, std::uint64_t maximum_size,
                    base::CancellationToken cancellation) const override;
    [[nodiscard]] base::Result<std::size_t> read(std::uint64_t offset,
                                                 std::span<std::byte> destination,
                                                 base::CancellationToken cancellation) override;

    [[nodiscard]] const FreeSkipPlan& plan() const noexcept;

  private:
    struct Impl;
    explicit FreeSkipBlockSource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class WindowsVolumeEnumerator final {
  public:
    [[nodiscard]] static base::Result<std::vector<WindowsVolumeInfo>> enumerate();
};

/// One partition from IOCTL_DISK_GET_DRIVE_LAYOUT_EX (for Manifest disks[]).
struct WindowsPartitionLayout final {
    std::uint32_t partition_number{0};
    std::uint64_t offset_bytes{0};
    std::uint64_t size_bytes{0};
    /// "MBR" | "GPT" | "RAW"
    std::string partition_style;
    bool is_active{false};
    std::uint8_t mbr_type{0};
    /// Lowercase UUID string; empty for MBR/RAW.
    std::string gpt_type_guid;
    std::string gpt_name;
    std::string volume_label;
    std::string filesystem;
    std::string volume_guid;
};

/// Raw MBR/GPT bytes captured for high-fidelity partition table restore.
struct WindowsRawDiskLayout final {
    std::vector<std::byte> mbr_sector;
    std::vector<std::byte> gpt_primary_header;
    std::vector<std::byte> gpt_partition_entries;
    std::vector<std::byte> gpt_backup_header;
    std::vector<std::byte> gpt_backup_entries;
};

/// Full physical disk layout for personal backup Manifest metadata.
struct WindowsPhysicalDiskLayout final {
    std::uint32_t disk_number{0};
    std::uint64_t disk_size_bytes{0};
    std::uint32_t bytes_per_sector{0};
    std::uint64_t total_sectors{0};
    /// "MBR" | "GPT" | "RAW"
    std::string partition_style;
    std::string model;
    std::string serial;
    std::string media_type;
    std::vector<WindowsPartitionLayout> partitions;
    WindowsRawDiskLayout raw_layout;
};

/// Opens \\.\PhysicalDrive{N} and reads size, style, model, partitions, and raw_layout.
[[nodiscard]] base::Result<WindowsPhysicalDiskLayout>
inspect_physical_disk_layout(std::uint32_t disk_number);

/// True when disk_number hosts the Windows system volume.
[[nodiscard]] base::Result<bool> is_system_physical_disk(std::uint32_t disk_number);

/// Deletes existing partition layout and validates capacity/sector size for raw disk restore.
[[nodiscard]] base::Result<void>
prepare_target_disk_for_raw_restore(std::uint32_t disk_number, std::uint64_t source_disk_size_bytes,
                                    std::uint32_t source_bytes_per_sector);

/// Writes captured MBR/GPT raw_layout onto the target physical disk.
[[nodiscard]] base::Result<void>
rebuild_partition_table_from_raw_layout(std::uint32_t disk_number,
                                        std::uint32_t source_bytes_per_sector,
                                        std::uint64_t source_disk_size_bytes,
                                        const WindowsRawDiskLayout& raw_layout,
                                        const std::string& partition_style);

/// When `preserve_disk_signature` is false, randomize MBR signature and/or GPT DiskId (and
/// recompute GPT header CRCs). When true, returns `layout` unchanged.
[[nodiscard]] base::Result<WindowsRawDiskLayout>
apply_disk_signature_policy(WindowsRawDiskLayout layout, bool preserve_disk_signature,
                            const std::string& partition_style);

/// After layout rebuild + online: grow the last data partition into free space when the target
/// is larger than `source_disk_size_bytes`, then extend NTFS/ReFS. FAT/exFAT leave free space
/// unallocated (partition not grown). Best-effort; returns success when there is nothing to do.
[[nodiscard]] base::Result<void>
expand_last_data_partition_on_disk(std::uint32_t disk_number,
                                   std::uint64_t source_disk_size_bytes,
                                   std::uint32_t bytes_per_sector,
                                   const std::string& partition_style);

/// Clears OFFLINE/READ_ONLY attributes so restored volumes can mount (data-disk path).
[[nodiscard]] base::Result<void> bring_target_disk_online(std::uint32_t disk_number);

/// Resolves which physical disk hosts a file path (for archive-on-target rejection).
[[nodiscard]] base::Result<std::uint32_t>
physical_disk_number_for_path(const std::filesystem::path& path);

class WindowsSourceInventory final : public ports::ISourceInventory {
  public:
    [[nodiscard]] base::Result<std::vector<ports::SourceInventoryRecord>>
    list_sources(base::CancellationToken cancellation) override;
};

} // namespace aegra::adapters::windows_disk
