#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aegra::ports {

/// Opaque snapshot-bound entry handle. Adapter-owned; invalid after snapshot/view destruction.
struct FileSnapshotEntryId final {
    std::uint64_t value{0};
};

struct FileEnumerateBatch final {
    std::vector<contracts::FileEntryDesc> entries;
    /// Opaque continuation for the next page; nullopt = end.
    std::optional<std::string> continuation_token;
};

/// Enumerates a filesystem-consistent snapshot view.
/// Thread safety: a single enumerator instance is not thread-safe.
/// Ownership: enumerator does not outlive the snapshot view that created it.
/// Cancel: long enumerate calls must observe cancellation within a bounded wait.
/// Destructor: no I/O; does not abort an in-flight parent job.
class IFileTreeEnumerator {
  public:
    IFileTreeEnumerator() = default;
    virtual ~IFileTreeEnumerator() = default;
    IFileTreeEnumerator(const IFileTreeEnumerator&) = delete;
    IFileTreeEnumerator& operator=(const IFileTreeEnumerator&) = delete;
    IFileTreeEnumerator(IFileTreeEnumerator&&) = delete;
    IFileTreeEnumerator& operator=(IFileTreeEnumerator&&) = delete;

    /// Returns the next batch in deterministic order. Empty entries with nullopt token = done.
    [[nodiscard]] virtual base::Result<FileEnumerateBatch>
    next_batch(std::uint32_t maximum_entries, base::CancellationToken cancellation) = 0;
};

/// Bound stream reader for one snapshot entry stream.
/// size_bytes() is stable for the reader lifetime.
/// read() may short-read at EOF; starting past EOF is an error.
/// Empty destination succeeds with 0.
/// Not thread-safe. Destructor closes handles; does not report cancel as success after mutation.
class IFileContentReader {
  public:
    IFileContentReader() = default;
    virtual ~IFileContentReader() = default;
    IFileContentReader(const IFileContentReader&) = delete;
    IFileContentReader& operator=(const IFileContentReader&) = delete;
    IFileContentReader(IFileContentReader&&) = delete;
    IFileContentReader& operator=(IFileContentReader&&) = delete;

    [[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;
    [[nodiscard]] virtual base::Result<std::size_t>
    read(std::uint64_t offset, std::span<std::byte> destination,
         base::CancellationToken cancellation) = 0;
};

/// Snapshot view for one or more volumes in a single consistent set.
/// open_reader binds stream identity; reader must not outlive the view.
/// Not thread-safe unless documented by the concrete adapter.
/// Destructor releases snapshot-relative handles; does not delete VSS (composition root owns VSS).
class IFileSnapshotView {
  public:
    IFileSnapshotView() = default;
    virtual ~IFileSnapshotView() = default;
    IFileSnapshotView(const IFileSnapshotView&) = delete;
    IFileSnapshotView& operator=(const IFileSnapshotView&) = delete;
    IFileSnapshotView(IFileSnapshotView&&) = delete;
    IFileSnapshotView& operator=(IFileSnapshotView&&) = delete;

    [[nodiscard]] virtual base::Result<std::unique_ptr<IFileTreeEnumerator>>
    open_enumerator(const contracts::FileSourceRef& selection,
                    base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<std::unique_ptr<IFileContentReader>>
    open_stream_reader(std::uint64_t entry_id, std::uint32_t stream_index,
                       base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
