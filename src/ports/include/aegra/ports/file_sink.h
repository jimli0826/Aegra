#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace aegra::ports {

struct FileSinkCapabilities final {
    bool supports_ads{false};
    bool supports_sparse{false};
    bool supports_security_descriptor{false};
    bool supports_reparse{false};
    bool supports_hard_link{false};
    std::uint64_t free_bytes{0};
};

/// Staged writer for one ordinary file (main stream and optional ADS).
/// write() consumes or copies payload before return; short writes are errors.
/// publish() atomically renames staging into place after metadata application policy.
/// Destructor aborts unpublished staging; never reports cancel after successful publish.
/// Not thread-safe.
class IStagedFileWriter {
  public:
    IStagedFileWriter() = default;
    virtual ~IStagedFileWriter() = default;
    IStagedFileWriter(const IStagedFileWriter&) = delete;
    IStagedFileWriter& operator=(const IStagedFileWriter&) = delete;
    IStagedFileWriter(IStagedFileWriter&&) = delete;
    IStagedFileWriter& operator=(IStagedFileWriter&&) = delete;

    [[nodiscard]] virtual base::Result<void>
    write(std::uint64_t offset, std::span<const std::byte> payload,
          base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void>
    set_sparse_ranges(const std::vector<contracts::FileAllocatedRangeDesc>& allocated,
                      base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void>
    write_alternate_stream(const contracts::EncodedName& name, std::span<const std::byte> payload,
                           base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void>
    apply_metadata(const contracts::FileEntryDesc& entry,
                   base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void> publish(contracts::FileConflictPolicy policy,
                                                     base::CancellationToken cancellation) = 0;

    virtual void abort() noexcept = 0;
};

/// Target-root-bound file tree sink. All operations use relative EncodedName components.
/// Constructor binds a verified root; no absolute path strings accepted.
/// Thread safety: single sink instance is not thread-safe.
/// Destructor cleans unpublished staging only; published mutations are not rolled back.
class IFileTreeSink {
  public:
    IFileTreeSink() = default;
    virtual ~IFileTreeSink() = default;
    IFileTreeSink(const IFileTreeSink&) = delete;
    IFileTreeSink& operator=(const IFileTreeSink&) = delete;
    IFileTreeSink(IFileTreeSink&&) = delete;
    IFileTreeSink& operator=(IFileTreeSink&&) = delete;

    [[nodiscard]] virtual base::Result<FileSinkCapabilities>
    capabilities(base::CancellationToken cancellation) const = 0;

    [[nodiscard]] virtual base::Result<void>
    create_directory(const std::vector<contracts::EncodedName>& relative_components,
                     base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<std::unique_ptr<IStagedFileWriter>>
    begin_file(const std::vector<contracts::EncodedName>& relative_components,
               std::uint64_t logical_size, base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void>
    create_hard_link(const std::vector<contracts::EncodedName>& existing_components,
                     const std::vector<contracts::EncodedName>& new_components,
                     base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void>
    create_reparse(const std::vector<contracts::EncodedName>& relative_components,
                   const contracts::FileEntryDesc& entry,
                   base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void>
    apply_directory_metadata(const std::vector<contracts::EncodedName>& relative_components,
                             const contracts::FileEntryDesc& entry,
                             base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void> flush(base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
