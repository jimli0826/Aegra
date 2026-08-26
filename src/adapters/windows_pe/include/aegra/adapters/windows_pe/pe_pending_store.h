#pragma once

#include "aegra/base/result.h"
#include "aegra/ports/pe_pending_store.h"

#include <memory>
#include <string>

namespace aegra::adapters::windows_pe {

/// Files live under `<data_dir>\pe\pending\`:
///   restore_job.v1.json   pending job (atomic replace; create-only occupancy)
///   restore_job.v1.key    sealed job key (restricted ACL; use-once)
///   restore_result.v1.json result written by the PE executor
struct PePendingStoreOpenRequest final {
    /// Absolute UTF-8 data directory (service default: %ProgramData%\Aegra).
    std::string data_dir_utf8;
};

/// Online side: open the store at a known data directory. Directories are created
/// on the first write.
[[nodiscard]] base::Result<std::unique_ptr<ports::IPePendingJobStore>>
open_pe_pending_store(const PePendingStoreOpenRequest& request);

struct LocatedPePendingStore final {
    std::unique_ptr<ports::IPePendingJobStore> store;
    /// UTF-8 `\\?\Volume{...}\ProgramData\Aegra` — the host data directory on the
    /// volume that holds the pending job. Set AEGRA_DATA_DIR to this before
    /// spawning the worker so its per-task log persists to the host across reboots.
    std::string data_dir_utf8;
};

/// WinPE side: drive letters are not stable, so scan all fixed volumes for
/// `\ProgramData\Aegra\pe\pending\restore_job.v1.json` and open the store on the
/// volume that holds it. Fails with kNotFound when no volume has a pending job and
/// with kConflict when more than one volume does (refuse ambiguity, never guess).
[[nodiscard]] base::Result<LocatedPePendingStore> locate_pe_pending_store();

} // namespace aegra::adapters::windows_pe
