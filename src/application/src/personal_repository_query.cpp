#include "aegra/application/personal_repository_query.h"

#include "aegra/format/manifest.h"

#include <utility>

namespace aegra::application {
namespace {

[[nodiscard]] contracts::PersonalBackupType map_backup_type(const format::BackupType type) {
    switch (type) {
    case format::BackupType::kFull:
        return contracts::PersonalBackupType::kFull;
    case format::BackupType::kIncremental:
        return contracts::PersonalBackupType::kIncremental;
    case format::BackupType::kDifferential:
        return contracts::PersonalBackupType::kDifferential;
    }
    return contracts::PersonalBackupType::kFull;
}

[[nodiscard]] contracts::RecoveryPointChainState
map_chain_state(const personal_repository::ChainState state) {
    return state == personal_repository::ChainState::kComplete
               ? contracts::RecoveryPointChainState::kComplete
               : contracts::RecoveryPointChainState::kIncomplete;
}

[[nodiscard]] contracts::RecoveryPointSummary
map_recovery_point(const personal_repository::CatalogRecoveryPoint& point) {
    const auto& entry = point.entry;
    return {
        .file_uuid = entry.file_uuid,
        .backup_set_uuid = entry.backup_set_uuid,
        .parent_uuid = entry.parent_uuid,
        .backup_type = map_backup_type(entry.backup_type),
        .chain_state = map_chain_state(point.chain_state),
        .created_utc_ms = entry.created_utc_ms,
        .logical_size_bytes = entry.logical_size_bytes,
        .stored_size_bytes = entry.stored_size_bytes,
        .source_count = entry.source_count,
        .has_sidecar = entry.has_sidecar,
    };
}

[[nodiscard]] contracts::RecoveryPointPage map_page(personal_repository::CatalogScanPage page) {
    contracts::RecoveryPointPage result;
    result.state = contracts::RepositoryCatalogState::kCatalogReady;
    result.repository_uuid = std::move(page.descriptor.repository_uuid);
    result.continuation_token = std::move(page.continuation_token);
    result.items.reserve(page.recovery_points.size());
    for (const auto& point : page.recovery_points) {
        result.items.push_back(map_recovery_point(point));
    }
    return result;
}

} // namespace

PersonalRepositoryQuery::PersonalRepositoryQuery() noexcept = default;

PersonalRepositoryQuery::PersonalRepositoryQuery(ports::IObjectReader& reader,
                                                 ports::IPrefixEnumerator& enumerator,
                                                 personal_repository::CatalogScannerLimits limits)
    : scanner_(std::make_unique<personal_repository::RepositoryCatalogScanner>(reader, enumerator,
                                                                               std::move(limits))) {
}

PersonalRepositoryQuery::~PersonalRepositoryQuery() = default;

base::Result<contracts::RecoveryPointPage>
PersonalRepositoryQuery::list_recovery_points(const contracts::RecoveryPointListRequest& request,
                                              const base::CancellationToken cancellation) {
    auto valid = contracts::validate_recovery_point_list_request(request);
    if (!valid) {
        return base::Result<contracts::RecoveryPointPage>::failure(valid.error());
    }
    if (!scanner_) {
        return base::Result<contracts::RecoveryPointPage>::success({});
    }
    auto scanned =
        scanner_->scan({request.continuation_token, request.maximum_results}, cancellation);
    if (!scanned) {
        return base::Result<contracts::RecoveryPointPage>::failure(scanned.error());
    }
    auto result = map_page(std::move(scanned).value());
    auto valid_result = contracts::validate_recovery_point_page(result);
    return valid_result ? base::Result<contracts::RecoveryPointPage>::success(std::move(result))
                        : base::Result<contracts::RecoveryPointPage>::failure(valid_result.error());
}

} // namespace aegra::application
