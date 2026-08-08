#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"
#include "aegra/contracts/job.h"
#include "aegra/ports/file_recovery_point.h"
#include "aegra/ports/file_source.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aegra::pipeline {

/// Classification of one current entry relative to parent + USN hints (FI3).
enum class FileEntryChangeClass : std::uint8_t {
    kNew = 1,
    kContent = 2,
    kMetadataOnly = 3,
    kRenameMove = 4,
    kUnchanged = 5,
    kAmbiguous = 6,
};

/// One stream that must be read from the snapshot and written as local payload.
struct FileLocalStreamWork final {
    std::uint64_t entry_id{0};
    std::uint32_t stream_index{0};
    std::size_t entry_pos{0};
    std::size_t stream_pos{0};
};

struct FileSetChangePlanResult final {
    /// Current tip File Index entries (complete tree); streams tagged local|parent.
    std::vector<contracts::FileEntryDesc> entries;
    /// Only local non-empty streams that need payload materialization.
    std::vector<FileLocalStreamWork> local_streams;
    std::uint64_t stream_count{0};
    std::uint64_t logical_bytes{0};
};

/// Inputs for the change planner. Eligibility (journal continuity, parent chain) is decided
/// by the composition root; when effective_type is Full, parent and hints are ignored.
struct FileSetChangePlannerRequest final {
    contracts::BackupType effective_type{contracts::BackupType::kFull};
    /// Non-owning; required when effective_type is Incremental.
    ports::IFileRecoveryPointReader* parent_reader{nullptr};
    /// Merged USN change hints for selected volumes (identity → reason). Empty is allowed
    /// only for Full; Incremental with empty hints treats every file as ambiguous (local).
    std::vector<contracts::FileChangeHint> change_hints;
    /// Memory budget for the compact parent identity index (bytes).
    std::size_t parent_index_budget_bytes{16U * 1024U * 1024U};
};

/// Plans stream content_storage for a fully enumerated current tree.
/// - Full: every main stream is local (existing Full planner semantics).
/// - Incremental: parent identity lookup + USN classification; ambiguous always local.
/// Does not load a full parent FileEntryDesc map; uses a compact sorted identity index.
[[nodiscard]] base::Result<FileSetChangePlanResult>
plan_file_set_streams(std::vector<contracts::FileEntryDesc> current_entries,
                      const FileSetChangePlannerRequest& request,
                      base::CancellationToken cancellation);

} // namespace aegra::pipeline
