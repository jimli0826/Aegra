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
};

struct WindowsBlockSinkOpenRequest final {
    std::filesystem::path path;
    WindowsBlockSinkKind kind{WindowsBlockSinkKind::kStableFile};
    std::optional<std::uint64_t> expected_capacity_bytes;
    std::vector<std::filesystem::path> protected_sources;
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
/// free_ranges are sorted, non-overlapping [start, end) byte ranges that may be synthesized as
/// zeros without reading the underlying device.
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

/// IBlockSource that zero-fills free ranges without I/O and forwards used ranges to an inner source.
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

class WindowsSourceInventory final : public ports::ISourceInventory {
  public:
    [[nodiscard]] base::Result<std::vector<ports::SourceInventoryRecord>>
    list_sources(base::CancellationToken cancellation) override;
};

} // namespace aegra::adapters::windows_disk
