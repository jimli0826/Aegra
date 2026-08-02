#include "aegra/personal_repository/chain_graph.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::personal_repository {
namespace {

[[nodiscard]] base::Error invalid(std::string message) {
    return {base::ErrorCode::kInvalidArgument, std::move(message)};
}

[[nodiscard]] base::Error conflict(std::string message) {
    return {base::ErrorCode::kConflict, std::move(message)};
}

[[nodiscard]] base::Error not_found() {
    return {base::ErrorCode::kNotFound, "recovery point does not exist"};
}

[[nodiscard]] base::Result<void> validate_known_parent(const CatalogEntry& child,
                                                       const CatalogEntry& parent) {
    if (child.backup_set_uuid != parent.backup_set_uuid) {
        return base::Result<void>::failure(conflict("recovery point parent crosses backup sets"));
    }
    if (child.backup_type == format::BackupType::kDifferential &&
        parent.backup_type != format::BackupType::kFull) {
        return base::Result<void>::failure(
            conflict("differential recovery point parent is not full"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
validate_known_edges(const std::map<std::string, CatalogEntry, std::less<>>& entries) {
    for (const auto& [uuid, entry] : entries) {
        static_cast<void>(uuid);
        if (!entry.parent_uuid) {
            continue;
        }
        const auto parent = entries.find(*entry.parent_uuid);
        if (parent == entries.end()) {
            continue;
        }
        auto valid = validate_known_parent(entry, parent->second);
        if (!valid) {
            return valid;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
detect_cycles(const std::map<std::string, CatalogEntry, std::less<>>& entries,
              const std::uint32_t maximum_depth) {
    for (const auto& [root_uuid, root] : entries) {
        static_cast<void>(root);
        std::set<std::string, std::less<>> visited;
        const CatalogEntry* current = &entries.at(root_uuid);
        std::uint32_t depth = 0;
        while (current != nullptr) {
            if (depth >= maximum_depth || !visited.insert(current->file_uuid).second) {
                return base::Result<void>::failure(
                    conflict("recovery point graph contains a cycle or excessive depth"));
            }
            ++depth;
            if (!current->parent_uuid) {
                break;
            }
            const auto parent = entries.find(*current->parent_uuid);
            if (parent == entries.end()) {
                break;
            }
            current = &parent->second;
        }
    }
    return base::Result<void>::success();
}

} // namespace

RecoveryPointGraph::RecoveryPointGraph(std::map<std::string, CatalogEntry, std::less<>> entries,
                                       const std::uint32_t maximum_chain_depth) noexcept
    : entries_(std::move(entries)), maximum_chain_depth_(maximum_chain_depth) {}

base::Result<RecoveryPointGraph>
RecoveryPointGraph::build(std::vector<CatalogEntry> entries,
                          const std::uint32_t maximum_chain_depth) {
    if (maximum_chain_depth == 0) {
        return base::Result<RecoveryPointGraph>::failure(
            invalid("recovery point graph input is invalid"));
    }
    std::map<std::string, CatalogEntry, std::less<>> indexed;
    const std::string repository_uuid =
        entries.empty() ? std::string{} : entries.front().repository_uuid;
    for (auto& entry : entries) {
        auto valid = validate_catalog_entry(entry);
        if (!valid) {
            return base::Result<RecoveryPointGraph>::failure(valid.error());
        }
        if (entry.repository_uuid != repository_uuid) {
            return base::Result<RecoveryPointGraph>::failure(
                conflict("recovery point graph crosses repositories"));
        }
        if (!indexed.emplace(entry.file_uuid, std::move(entry)).second) {
            return base::Result<RecoveryPointGraph>::failure(
                conflict("recovery point graph contains a duplicate UUID"));
        }
    }
    auto edges = validate_known_edges(indexed);
    auto cycles = edges ? detect_cycles(indexed, maximum_chain_depth) : edges;
    if (!cycles) {
        return base::Result<RecoveryPointGraph>::failure(cycles.error());
    }
    return base::Result<RecoveryPointGraph>::success(
        RecoveryPointGraph(std::move(indexed), maximum_chain_depth));
}

const CatalogEntry* RecoveryPointGraph::find(const std::string_view file_uuid) const noexcept {
    const auto found = entries_.find(file_uuid);
    return found == entries_.end() ? nullptr : &found->second;
}

base::Result<std::vector<CatalogEntry>>
RecoveryPointGraph::resolve_chain(const std::string_view file_uuid) const {
    const auto* current = find(file_uuid);
    if (current == nullptr) {
        return base::Result<std::vector<CatalogEntry>>::failure(not_found());
    }
    std::vector<CatalogEntry> reversed;
    reversed.reserve(maximum_chain_depth_);
    while (current != nullptr && reversed.size() < maximum_chain_depth_) {
        reversed.push_back(*current);
        if (!current->parent_uuid) {
            break;
        }
        current = find(*current->parent_uuid);
    }
    if (current == nullptr || reversed.back().backup_type != format::BackupType::kFull) {
        return base::Result<std::vector<CatalogEntry>>::failure(
            conflict("recovery point chain is incomplete"));
    }
    if (reversed.size() == maximum_chain_depth_ && reversed.back().parent_uuid) {
        return base::Result<std::vector<CatalogEntry>>::failure(
            conflict("recovery point chain exceeds maximum depth"));
    }
    std::reverse(reversed.begin(), reversed.end());
    return base::Result<std::vector<CatalogEntry>>::success(std::move(reversed));
}

base::Result<ChainState> RecoveryPointGraph::chain_state(const std::string_view file_uuid) const {
    const auto* entry = find(file_uuid);
    if (entry == nullptr) {
        return base::Result<ChainState>::failure(not_found());
    }
    auto chain = resolve_chain(file_uuid);
    return base::Result<ChainState>::success(chain ? ChainState::kComplete
                                                   : ChainState::kIncomplete);
}

} // namespace aegra::personal_repository
