#include "aegra/personal_repository/retention.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aegra::personal_repository {
namespace {

[[nodiscard]] base::Error invalid(std::string message) {
    return {base::ErrorCode::kInvalidArgument, std::move(message)};
}

[[nodiscard]] base::Error not_found() {
    return {base::ErrorCode::kNotFound, "recovery point does not exist"};
}

} // namespace

base::Result<RetentionAncestorClosure>
compute_retention_ancestor_closure(const RecoveryPointGraph& graph,
                                   const std::span<const std::string_view> retained_tip_file_uuids,
                                   const std::uint32_t full_request_chain_depth_threshold) {
    RetentionAncestorClosure result;
    std::set<std::string, std::less<>> unique;
    for (const auto tip_uuid : retained_tip_file_uuids) {
        if (tip_uuid.empty()) {
            return base::Result<RetentionAncestorClosure>::failure(
                invalid("retention tip identity is invalid"));
        }
        auto chain = graph.resolve_chain(tip_uuid);
        if (!chain) {
            if (chain.error().code == base::ErrorCode::kNotFound) {
                return base::Result<RetentionAncestorClosure>::failure(not_found());
            }
            return base::Result<RetentionAncestorClosure>::failure(chain.error());
        }
        const auto depth = static_cast<std::uint32_t>(chain.value().size());
        if (depth > result.deepest_chain_depth) {
            result.deepest_chain_depth = depth;
        }
        if (full_request_chain_depth_threshold != 0 &&
            depth >= full_request_chain_depth_threshold) {
            result.request_new_full = true;
        }
        for (const auto& layer : chain.value()) {
            unique.insert(layer.file_uuid);
        }
    }
    result.retain_file_uuids.assign(unique.begin(), unique.end());
    return base::Result<RetentionAncestorClosure>::success(std::move(result));
}

base::Result<std::vector<std::string>>
select_recent_recovery_point_tips(const std::span<const CatalogEntry> entries,
                                  const std::string_view backup_set_uuid,
                                  const std::string_view content_kind,
                                  const std::uint32_t keep_count) {
    if (backup_set_uuid.empty() || content_kind.empty() || keep_count == 0) {
        return base::Result<std::vector<std::string>>::failure(
            invalid("retention tip selection request is invalid"));
    }
    std::vector<const CatalogEntry*> candidates;
    candidates.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.backup_set_uuid == backup_set_uuid && entry.content_kind == content_kind &&
            entry.structural_state == "complete") {
            candidates.push_back(&entry);
        }
    }
    std::ranges::sort(candidates, [](const CatalogEntry* left, const CatalogEntry* right) {
        if (left->created_utc_ms != right->created_utc_ms) {
            return left->created_utc_ms > right->created_utc_ms;
        }
        return left->file_uuid > right->file_uuid;
    });
    if (candidates.size() > keep_count) {
        candidates.resize(keep_count);
    }
    std::vector<std::string> tips;
    tips.reserve(candidates.size());
    for (const auto* entry : candidates) {
        tips.push_back(entry->file_uuid);
    }
    return base::Result<std::vector<std::string>>::success(std::move(tips));
}

bool recovery_point_has_catalog_descendants(const std::span<const CatalogEntry> entries,
                                            const std::string_view file_uuid) noexcept {
    if (file_uuid.empty()) {
        return false;
    }
    for (const auto& entry : entries) {
        if (entry.parent_uuid && *entry.parent_uuid == file_uuid) {
            return true;
        }
    }
    return false;
}

} // namespace aegra::personal_repository
