#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"
#include "aegra/ports/file_backup_session.h"
#include "aegra/ports/file_source.h"
#include "aegra/ports/progress.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aegra::pipeline {

struct FileSetBackupPlan final {
    std::string job_id;
    std::string trace_id;
    std::vector<contracts::FileSourceRef> selections;
    /// Stream payload quantum (must match Archive block_size).
    std::uint32_t block_size_bytes{4096};
    /// Maximum single read/write payload size (multiple of block_size).
    std::uint32_t chunk_size_bytes{4U * 1024U * 1024U};
    std::size_t memory_budget_bytes{64U * 1024U * 1024U};
    std::uint32_t enumerate_batch_size{256};
};

struct FileSetBackupSummary final {
    std::uint64_t entry_count{0};
    std::uint64_t stream_count{0};
    std::uint64_t logical_bytes{0};
    std::uint64_t stored_bytes{0};
    std::uint64_t chunk_count{0};
    std::uint64_t processed_entries{0};
};

/// Platform-agnostic file_set backup orchestration.
/// Depends only on ports (snapshot view + file backup session); no Win32/Archive types.
class FileSetBackupPipeline final {
  public:
    FileSetBackupPipeline(ports::IFileSnapshotView& snapshot, ports::IFileBackupSession& session,
                          ports::IProgressSink* progress = nullptr) noexcept;

    [[nodiscard]] base::Result<FileSetBackupSummary>
    run(const FileSetBackupPlan& plan, const base::CancellationToken& cancellation);

  private:
    ports::IFileSnapshotView& snapshot_;
    ports::IFileBackupSession& session_;
    ports::IProgressSink* progress_;
};

} // namespace aegra::pipeline
