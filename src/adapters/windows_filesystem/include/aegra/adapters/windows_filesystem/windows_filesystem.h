#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"
#include "aegra/ports/file_browser.h"
#include "aegra/ports/file_sink.h"
#include "aegra/ports/file_source.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegra::adapters::windows_filesystem {

/// Composition-root mapping: durable volume identity -> snapshot root path (UTF-16).
/// Adapter does not create VSS; the Worker/Service injects resolved snapshot roots.
struct SnapshotVolumeBinding final {
    std::string volume_identity;
    /// Absolute snapshot device/root path as UTF-16LE code units (no trailing null required).
    std::vector<std::uint16_t> snapshot_root_utf16;
    /// Non-authoritative UI label for volume roots (Service/Worker may leave empty).
    std::string display_name;
};

struct WindowsFileSnapshotOpenRequest final {
    std::vector<SnapshotVolumeBinding> volumes;
};

/// Snapshot-bound file tree source implementing IFileSnapshotView.
class WindowsFileSnapshotView final : public ports::IFileSnapshotView {
  public:
    ~WindowsFileSnapshotView() override;
    WindowsFileSnapshotView(const WindowsFileSnapshotView&) = delete;
    WindowsFileSnapshotView& operator=(const WindowsFileSnapshotView&) = delete;
    WindowsFileSnapshotView(WindowsFileSnapshotView&&) = delete;
    WindowsFileSnapshotView& operator=(WindowsFileSnapshotView&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<WindowsFileSnapshotView>>
    open(const WindowsFileSnapshotOpenRequest& request);

    [[nodiscard]] base::Result<std::unique_ptr<ports::IFileTreeEnumerator>>
    open_enumerator(const contracts::FileSourceRef& selection,
                    base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<std::unique_ptr<ports::IFileContentReader>>
    open_stream_reader(std::uint64_t entry_id, std::uint32_t stream_index,
                       base::CancellationToken cancellation) override;

  private:
    struct Impl;
    explicit WindowsFileSnapshotView(std::unique_ptr<Impl> implementation) noexcept;
    std::unique_ptr<Impl> implementation_;
};

struct WindowsFileTreeSinkOpenRequest final {
    /// Absolute target root as UTF-16LE code units.
    std::vector<std::uint16_t> target_root_utf16;
};

/// Restore sink for a Windows NTFS/ReFS/FAT32 target root.
/// FAT32 reports no security-descriptor support and a 4 GiB - 1 per-file limit.
class WindowsFileTreeSink final : public ports::IFileTreeSink {
  public:
    ~WindowsFileTreeSink() override;
    WindowsFileTreeSink(const WindowsFileTreeSink&) = delete;
    WindowsFileTreeSink& operator=(const WindowsFileTreeSink&) = delete;
    WindowsFileTreeSink(WindowsFileTreeSink&&) = delete;
    WindowsFileTreeSink& operator=(WindowsFileTreeSink&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<WindowsFileTreeSink>>
    open(const WindowsFileTreeSinkOpenRequest& request);

    [[nodiscard]] base::Result<ports::FileSinkCapabilities>
    capabilities(base::CancellationToken cancellation) const override;

    [[nodiscard]] base::Result<void>
    create_directory(const std::vector<contracts::EncodedName>& relative_components,
                     base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<std::unique_ptr<ports::IStagedFileWriter>>
    begin_file(const std::vector<contracts::EncodedName>& relative_components,
               std::uint64_t logical_size, base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<void>
    apply_directory_metadata(const std::vector<contracts::EncodedName>& relative_components,
                             const contracts::FileEntryDesc& entry,
                             base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<void> flush(base::CancellationToken cancellation) override;

  private:
    struct Impl;
    explicit WindowsFileTreeSink(std::unique_ptr<Impl> implementation) noexcept;
    std::unique_ptr<Impl> implementation_;
};

/// Live (non-snapshot) browse helper for Service. Node tokens are opaque decimal handles.
/// Full token TTL/authorization is enforced by Service; this adapter only enumerates FS nodes.
class WindowsFileSourceBrowser final : public ports::IFileSourceBrowser {
  public:
    ~WindowsFileSourceBrowser() override;
    WindowsFileSourceBrowser(const WindowsFileSourceBrowser&) = delete;
    WindowsFileSourceBrowser& operator=(const WindowsFileSourceBrowser&) = delete;
    WindowsFileSourceBrowser(WindowsFileSourceBrowser&&) = delete;
    WindowsFileSourceBrowser& operator=(WindowsFileSourceBrowser&&) = delete;

    /// roots: volume_identity -> live root path UTF-16 for authorized volumes.
    [[nodiscard]] static base::Result<std::unique_ptr<WindowsFileSourceBrowser>>
    create(std::vector<SnapshotVolumeBinding> roots);

    [[nodiscard]] base::Result<contracts::FileSourceNodePage>
    list_children(const ports::FileBrowseSession& session,
                  const std::optional<std::string>& parent_node_token,
                  const contracts::ServicePageRequest& page, bool include_unavailable,
                  base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<contracts::FileSourceRef>
    resolve_selection(const ports::FileBrowseSession& session, const std::string& node_token,
                      contracts::FileRecursion recursion, const std::string& display_label,
                      base::CancellationToken cancellation) override;

  private:
    struct Impl;
    explicit WindowsFileSourceBrowser(std::unique_ptr<Impl> implementation) noexcept;
    std::unique_ptr<Impl> implementation_;
};

} // namespace aegra::adapters::windows_filesystem
