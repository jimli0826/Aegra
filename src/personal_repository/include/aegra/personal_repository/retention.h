#pragma once

#include "aegra/base/result.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/chain_graph.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::personal_repository {

/// Retention never rewrites chains. Keep each retained tip plus every Catalog ancestor to Full.
struct RetentionAncestorClosure final {
    /// Unique file_uuid values that must remain (tips and their ancestors), sorted ascending.
    std::vector<std::string> retain_file_uuids;
    /// Deepest tip→Full chain length among retained tips (1 = tip is Full).
    std::uint32_t deepest_chain_depth{0};
    /// True when any retained tip chain depth is >= full_request_chain_depth_threshold
    /// (threshold 0 disables the flag). Callers may schedule a new Full before pruning.
    bool request_new_full{false};
};

/// Expand retained tip UUIDs to the ancestor closure required for restore integrity.
/// Missing tip or incomplete chain → failure (do not silently drop ancestors).
[[nodiscard]] base::Result<RetentionAncestorClosure>
compute_retention_ancestor_closure(const RecoveryPointGraph& graph,
                                   std::span<const std::string_view> retained_tip_file_uuids,
                                   std::uint32_t full_request_chain_depth_threshold = 0);

/// Select the N most recent recovery points (by created_utc_ms, then file_uuid) in one
/// backup set and content_kind. Does not expand ancestors — pair with the closure API.
[[nodiscard]] base::Result<std::vector<std::string>>
select_recent_recovery_point_tips(std::span<const CatalogEntry> entries,
                                  std::string_view backup_set_uuid,
                                  std::string_view content_kind, std::uint32_t keep_count);

/// True when deleting only `file_uuid` would leave Catalog descendants that still reference it.
/// Deletion plans must use the full descendant-first subtree (see plan_delete_recovery_points).
[[nodiscard]] bool recovery_point_has_catalog_descendants(
    std::span<const CatalogEntry> entries, std::string_view file_uuid) noexcept;

} // namespace aegra::personal_repository
