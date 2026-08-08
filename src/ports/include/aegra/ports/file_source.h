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
///
/// FI1 journal Port (implemented by FI2 Windows adapter):
/// - query_journal_state / read_change_batch must use the same snapshot-consistent view as
///   enumeration; must not fall back to the live volume when snapshot journal is unavailable.
/// - read_change_batch covers half-open [start_usn, end_usn); short batches set next_start_usn.
/// - Ownership: returned batches are value copies; no HANDLE/USN_RECORD exposure.
/// - Cancel: long journal reads observe cancellation within a bounded wait.
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

    /// Returns journal state for one volume_identity in this snapshot set.
    /// available=false is not a hard failure of the view; callers may Full-downgrade.
    [[nodiscard]] virtual base::Result<contracts::FileJournalState>
    query_journal_state(const std::string& volume_identity,
                        base::CancellationToken cancellation) = 0;

    /// Reads a bounded change batch for [start_usn, end_usn) on volume_identity.
    /// maximum_hints caps returned hints (must be 1..kMaximumChangeHintsPerBatch).
    [[nodiscard]] virtual base::Result<contracts::FileChangeBatch>
    read_change_batch(const std::string& volume_identity, std::int64_t start_usn,
                      std::int64_t end_usn, std::uint32_t maximum_hints,
                      base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
