#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::ports {

struct FileChunkWriteRequest final {
    std::uint64_t chunk_index{0};
    std::uint32_t stream_index{0};
    std::uint64_t logical_block_index{0};
    /// Uncompressed size of this block; must equal payload.size().
    std::uint32_t logical_size{0};
    /// Reserved; session chooses RAW vs COMPRESSED from logical payload (V7 zstd).
    std::uint8_t block_flags{0};
    /// Logical (uncompressed) block bytes for the lifetime of the call.
    std::span<const std::byte> payload;
};

/// Streaming file_set Archive session (Personal Archive V7).
/// Accepts entry metadata and stream chunks without retaining the full tree in memory.
/// Index pages are staged to a spool by the implementation (location owned by composition root).
///
/// Thread safety: single-threaded write API unless the implementation documents otherwise.
/// Payload lifetime: write_* must consume or copy span data before return.
/// Cancel: write/finalize observe cancellation; after successful commit, cancel is ignored.
/// Destructor: if not committed, aborts and deletes spool + partial archive group (best effort).
class IFileBackupSession {
  public:
    IFileBackupSession() = default;
    virtual ~IFileBackupSession() = default;
    IFileBackupSession(const IFileBackupSession&) = delete;
    IFileBackupSession& operator=(const IFileBackupSession&) = delete;
    IFileBackupSession(IFileBackupSession&&) = delete;
    IFileBackupSession& operator=(IFileBackupSession&&) = delete;

    [[nodiscard]] virtual base::Result<void>
    write_entry(const contracts::FileEntryDesc& entry, base::CancellationToken cancellation) = 0;

    /// Writes one stream chunk. Payload is always logical/uncompressed; the session may
    /// compress (zstd) before store. On success returns stored payload bytes (pre-AEAD body).
    [[nodiscard]] virtual base::Result<std::uint64_t>
    write_stream_chunk(const FileChunkWriteRequest& request,
                       base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void> finalize(base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void> commit(base::CancellationToken cancellation) = 0;

    virtual void abort() noexcept = 0;
};

} // namespace aegra::ports
