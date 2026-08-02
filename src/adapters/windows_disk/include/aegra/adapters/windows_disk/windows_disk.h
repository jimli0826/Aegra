#pragma once

#include "aegra/ports/block_io.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aegra::adapters::windows_disk {

enum class WindowsBlockSourceKind {
    kStableFile,
    kVssSnapshot,
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

    [[nodiscard]] std::uint64_t size_bytes() const noexcept override;
    [[nodiscard]] base::Result<std::size_t> read(std::uint64_t offset,
                                                 std::span<std::byte> destination,
                                                 base::CancellationToken cancellation) override;

  private:
    struct Impl;

    explicit WindowsBlockSource(std::unique_ptr<Impl> impl);

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
    std::uint32_t cluster_size_bytes{0};
    std::vector<WindowsVolumeExtent> extents;
    bool filesystem_metadata_available{false};
    bool volume_size_available{false};
    bool disk_extents_available{false};
};

class WindowsVolumeEnumerator final {
  public:
    [[nodiscard]] static base::Result<std::vector<WindowsVolumeInfo>> enumerate();
};

} // namespace aegra::adapters::windows_disk
