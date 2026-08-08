#pragma once

#include "aegra/contracts/file_set.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/chain_graph.h"

#include <span>
#include <string>
#include <string_view>

namespace aegra::personal_repository {

/// Inputs for file_set Incremental parent selection (Catalog graph only; no Archive open).
/// Parent candidate is exclusively schedules.last_recovery_point_id — never a Catalog tip scan.
struct FileIncrementalParentRequest final {
    std::string_view last_recovery_point_id;
    std::string_view schedule_backup_set_uuid;
    /// Lowercase 64-char hex selection fingerprint for the current Schedule selections.
    std::string_view expected_selection_fingerprint;
};

/// Pure Catalog decision: either hang Incremental on the tip, or demote to Full with a reason.
/// Never skips to an older ancestor when the tip is structurally unqualified.
struct FileIncrementalParentDecision final {
    bool incremental{false};
    /// Non-owning pointer into the graph when incremental is true; otherwise nullptr.
    const CatalogEntry* parent{nullptr};
    contracts::IncrementalDowngradeReason reason{contracts::IncrementalDowngradeReason::kNone};
};

/// Evaluate whether last_rp is a legal file_set Incremental parent under graph rules.
/// Hard IO/decode errors are the caller's responsibility (load graph first). This API only
/// classifies structural/eligibility failures as Full downgrades with stable reasons.
[[nodiscard]] FileIncrementalParentDecision
select_file_incremental_parent(const RecoveryPointGraph& graph,
                               const FileIncrementalParentRequest& request) noexcept;

/// Volume parent eligibility: sidecar + complete structural state + ordered source volume ids.
[[nodiscard]] bool is_volume_chainable_parent(const CatalogEntry& entry,
                                              std::span<const std::string> source_volume_ids,
                                              std::string_view schedule_backup_set_uuid) noexcept;

} // namespace aegra::personal_repository
