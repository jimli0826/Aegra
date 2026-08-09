#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"
#include "aegra/ports/file_recovery_point.h"
#include "aegra/ports/file_sink.h"
#include "aegra/ports/progress.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::pipeline {

struct FileSetRestorePlan final {
    std::string job_id;
    std::string trace_id;
    /// Empty = restore every entry under parent 0 roots (full tree).
    /// Non-empty = selection seeds; Pipeline expands directory seeds to all reachable
    /// descendants and includes path ancestors (FILE_SET_BACKUP_RESTORE §12).
    std::vector<std::uint64_t> entry_ids;
    contracts::FileConflictPolicy conflict_policy{contracts::FileConflictPolicy::kFail};
    bool restore_security{true};
    std::uint32_t read_buffer_bytes{4U * 1024U * 1024U};
};

struct FileSetRestoreSummary final {
    contracts::PartialRestoreStats stats;
    std::uint64_t directories_created{0};
    std::uint64_t files_published{0};
    /// Denominator for live TaskProgress percent (sum of selected file logical sizes).
    /// Zero means byte percent is unknown (directory-only selection).
    std::uint64_t progress_logical_bytes{0};
};

/// Platform-agnostic file_set restore. Completes Index selection preflight before any Sink mutation.
class FileSetRestorePipeline final {
  public:
    FileSetRestorePipeline(ports::IFileRecoveryPointReader& reader, ports::IFileTreeSink& sink,
                           ports::IProgressSink* progress = nullptr) noexcept;

    [[nodiscard]] base::Result<FileSetRestoreSummary>
    run(const FileSetRestorePlan& plan, const base::CancellationToken& cancellation);

  private:
    ports::IFileRecoveryPointReader& reader_;
    ports::IFileTreeSink& sink_;
    ports::IProgressSink* progress_;
};

} // namespace aegra::pipeline
