#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"
#include "aegra/contracts/service_control.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aegra::ports {

struct FileEntryPage final {
    std::vector<contracts::RecoveryPointEntrySummary> items;
    std::optional<std::string> continuation_token;
};

struct FileStreamReadRequest final {
    std::uint32_t stream_index{0};
    std::uint64_t offset{0};
    std::uint64_t size{0};
};

/// Authenticated file_set Recovery Point reader (Archive V7 File Index).
/// No Archive physical paths or offsets are exposed through this interface.
///
/// Thread safety: one reader instance is not thread-safe.
/// Descriptor stability: entry graph and stream sizes are stable for the reader lifetime.
/// read_stream may short-read at EOF; must authenticate before returning payload bytes.
/// Cancel: list/read observe cancellation; destructor releases caches only.
class IFileRecoveryPointReader {
  public:
    IFileRecoveryPointReader() = default;
    virtual ~IFileRecoveryPointReader() = default;
    IFileRecoveryPointReader(const IFileRecoveryPointReader&) = delete;
    IFileRecoveryPointReader& operator=(const IFileRecoveryPointReader&) = delete;
    IFileRecoveryPointReader(IFileRecoveryPointReader&&) = delete;
    IFileRecoveryPointReader& operator=(IFileRecoveryPointReader&&) = delete;

    [[nodiscard]] virtual std::string index_root_digest() const = 0;
    [[nodiscard]] virtual std::uint64_t entry_count() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t stream_count() const noexcept = 0;

    [[nodiscard]] virtual base::Result<FileEntryPage>
    list_children(std::uint64_t parent_entry_id, std::uint32_t maximum_results,
                  const std::optional<std::string>& continuation_token,
                  base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<contracts::FileEntryDesc>
    describe_entry(std::uint64_t entry_id, base::CancellationToken cancellation) = 0;

    /// Reads authenticated stream bytes into destination; returns bytes written.
    [[nodiscard]] virtual base::Result<std::size_t>
    read_stream(const FileStreamReadRequest& request, std::span<std::byte> destination,
                base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
