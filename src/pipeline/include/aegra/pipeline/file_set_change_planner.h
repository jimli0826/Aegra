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

/// Inputs for the change planner. Parent-chain eligibility is decided by the composition root.
struct FileSetChangePlannerRequest final {
    contracts::BackupType effective_type{contracts::BackupType::kFull};
    /// Non-owning; required when effective_type is Incremental.
    ports::IFileRecoveryPointReader* parent_reader{nullptr};
    /// Memory budget for the compact parent path index (bytes).
    std::size_t parent_index_budget_bytes{16U * 1024U * 1024U};
};

/// Plans stream content_storage for a fully enumerated current tree.
/// - Full: every main stream is local (existing Full planner semantics).
/// - Incremental: same-path regular files reuse the direct parent stream only when
///   write_time and logical_size both match.
/// Does not load a full parent FileEntryDesc map; uses a bounded sorted path index.
[[nodiscard]] base::Result<FileSetChangePlanResult>
plan_file_set_streams(std::vector<contracts::FileEntryDesc> current_entries,
                      const FileSetChangePlannerRequest& request,
                      base::CancellationToken cancellation);

} // namespace aegra::pipeline
