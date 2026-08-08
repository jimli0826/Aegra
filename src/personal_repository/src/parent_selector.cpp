#include "aegra/personal_repository/parent_selector.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace aegra::personal_repository {
namespace {

using contracts::IncrementalDowngradeReason;

[[nodiscard]] FileIncrementalParentDecision demote(const IncrementalDowngradeReason reason) {
    return FileIncrementalParentDecision{false, nullptr, reason};
}

[[nodiscard]] bool valid_hex_fingerprint(const std::string_view value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    return std::ranges::all_of(value, [](const unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

[[nodiscard]] bool fingerprints_compatible(const CatalogEntry& entry,
                                           const std::string_view expected) noexcept {
    return !entry.file_selection_fingerprint.empty() &&
           entry.file_selection_fingerprint == expected;
}

enum class ChainSelectionMatch : std::uint8_t {
    kOk = 0,
    kBaselineMissing = 1,
    kSelectionMismatch = 2,
    kStructureInvalid = 3,
};

[[nodiscard]] ChainSelectionMatch
classify_chain_selection(const std::vector<CatalogEntry>& chain, const std::string_view expected,
                       const std::string_view backup_set) noexcept {
    for (const auto& layer : chain) {
        if (layer.content_kind != kCatalogContentKindFileSet ||
            layer.backup_set_uuid != backup_set || layer.structural_state != "complete") {
            return ChainSelectionMatch::kStructureInvalid;
        }
        if (layer.file_selection_fingerprint.empty()) {
            return ChainSelectionMatch::kBaselineMissing;
        }
        if (layer.file_selection_fingerprint != expected) {
            return ChainSelectionMatch::kSelectionMismatch;
        }
    }
    return ChainSelectionMatch::kOk;
}

} // namespace

FileIncrementalParentDecision
select_file_incremental_parent(const RecoveryPointGraph& graph,
                               const FileIncrementalParentRequest& request) noexcept {
    if (request.schedule_backup_set_uuid.empty() ||
        !valid_hex_fingerprint(request.expected_selection_fingerprint)) {
        return demote(IncrementalDowngradeReason::kBaselineInvalid);
    }
    if (request.last_recovery_point_id.empty()) {
        return demote(IncrementalDowngradeReason::kNoParent);
    }

    const auto* tip = graph.find(request.last_recovery_point_id);
    if (tip == nullptr) {
        // Tip deleted or never published — do not rescan Catalog for an older ancestor.
        return demote(IncrementalDowngradeReason::kNoParent);
    }
    if (tip->content_kind != kCatalogContentKindFileSet || tip->structural_state != "complete" ||
        tip->backup_set_uuid != request.schedule_backup_set_uuid ||
        (tip->backup_type != format::BackupType::kFull &&
         tip->backup_type != format::BackupType::kIncremental)) {
        return demote(IncrementalDowngradeReason::kChainIncomplete);
    }
    if (!tip->file_baseline_available || tip->file_selection_fingerprint.empty()) {
        return demote(IncrementalDowngradeReason::kBaselineInvalid);
    }
    if (!fingerprints_compatible(*tip, request.expected_selection_fingerprint)) {
        return demote(IncrementalDowngradeReason::kSelectionChanged);
    }

    auto chain = graph.resolve_chain(request.last_recovery_point_id);
    if (!chain) {
        return demote(IncrementalDowngradeReason::kChainIncomplete);
    }
    switch (classify_chain_selection(chain.value(), request.expected_selection_fingerprint,
                                     request.schedule_backup_set_uuid)) {
    case ChainSelectionMatch::kOk:
        break;
    case ChainSelectionMatch::kBaselineMissing:
        return demote(IncrementalDowngradeReason::kBaselineInvalid);
    case ChainSelectionMatch::kSelectionMismatch:
        return demote(IncrementalDowngradeReason::kSelectionChanged);
    case ChainSelectionMatch::kStructureInvalid:
        return demote(IncrementalDowngradeReason::kChainIncomplete);
    }

    FileIncrementalParentDecision decision;
    decision.incremental = true;
    decision.parent = tip;
    decision.reason = IncrementalDowngradeReason::kNone;
    return decision;
}

bool is_volume_chainable_parent(const CatalogEntry& entry,
                               const std::span<const std::string> source_volume_ids,
                               const std::string_view schedule_backup_set_uuid) noexcept {
    if (entry.content_kind != kCatalogContentKindVolumeSet ||
        entry.backup_set_uuid != schedule_backup_set_uuid || !entry.has_sidecar ||
        entry.structural_state != "complete") {
        return false;
    }
    if (entry.backup_type != format::BackupType::kFull &&
        entry.backup_type != format::BackupType::kIncremental) {
        return false;
    }
    if (entry.source_volume_ids.size() != source_volume_ids.size()) {
        return false;
    }
    return std::ranges::equal(entry.source_volume_ids, source_volume_ids);
}

} // namespace aegra::personal_repository
