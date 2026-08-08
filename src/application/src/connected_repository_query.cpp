#include "aegra/application/connected_repository_query.h"

#include "aegra/format/manifest.h"

#include <optional>
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

[[nodiscard]] contracts::ContentKind map_content_kind(const std::string& kind) noexcept {
    return kind == personal_repository::kCatalogContentKindFileSet
               ? contracts::ContentKind::kFileSet
               : contracts::ContentKind::kVolumeSet;
}

[[nodiscard]] contracts::RecoveryPointSummary
map_recovery_point(const personal_repository::CatalogRecoveryPoint& point) {
    const auto& entry = point.entry;
    return {
        .file_uuid = entry.file_uuid,
        .backup_set_uuid = entry.backup_set_uuid,
        .parent_uuid = entry.parent_uuid,
        .backup_type = map_backup_type(entry.backup_type),
        .content_kind = map_content_kind(entry.content_kind),
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

[[nodiscard]] base::Result<std::optional<ports::RepositoryConnectionRecord>>
resolve_connection(ports::IControlPlaneDatabase& control_plane,
                   const std::optional<std::string>& connection_id,
                   const base::CancellationToken cancellation) {
    if (connection_id) {
        return control_plane.get_repository_connection(*connection_id, cancellation);
    }
    auto page = control_plane.list_repository_connections(
        {{contracts::kMaximumServicePageResults, std::nullopt}, std::nullopt}, cancellation);
    if (!page) {
        return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
            page.error());
    }
    for (const auto& summary : page.value().items) {
        if (!summary.is_default) {
            continue;
        }
        return control_plane.get_repository_connection(summary.connection_id, cancellation);
    }
    // No default: treat as not configured for multi-connection query.
    return base::Result<std::optional<ports::RepositoryConnectionRecord>>::success(std::nullopt);
}

} // namespace

ConnectedRepositoryQuery::ConnectedRepositoryQuery(
    ports::IControlPlaneDatabase& control_plane, ports::IRepositoryStorageFactory& storage_factory,
    personal_repository::CatalogScannerLimits limits) noexcept
    : control_plane_(control_plane), storage_factory_(storage_factory), limits_(std::move(limits)) {
}

base::Result<contracts::ServiceRecoveryPointPage> ConnectedRepositoryQuery::list_recovery_points(
    const contracts::ServiceRecoveryPointListRequest& request,
    const base::CancellationToken cancellation) {
    auto valid = contracts::validate_service_recovery_point_list_request(request);
    if (!valid) {
        return base::Result<contracts::ServiceRecoveryPointPage>::failure(valid.error());
    }
    auto connection =
        resolve_connection(control_plane_, request.repository_connection_id, cancellation);
    if (!connection) {
        return base::Result<contracts::ServiceRecoveryPointPage>::failure(connection.error());
    }
    if (!connection.value()) {
        contracts::ServiceRecoveryPointPage page;
        page.repository_connection_id = request.repository_connection_id;
        page.catalog.state = contracts::RepositoryCatalogState::kNotConfigured;
        return base::Result<contracts::ServiceRecoveryPointPage>::success(std::move(page));
    }

    auto storage = storage_factory_.open(connection.value()->locator, cancellation);
    if (!storage) {
        if (storage.error().code != base::ErrorCode::kNotFound &&
            storage.error().code != base::ErrorCode::kIoFailure) {
            return base::Result<contracts::ServiceRecoveryPointPage>::failure(storage.error());
        }
        // Offline or missing root is a structured unavailable catalog.
        contracts::ServiceRecoveryPointPage page;
        page.repository_connection_id = connection.value()->connection_id;
        page.catalog.state = contracts::RepositoryCatalogState::kNotConfigured;
        return base::Result<contracts::ServiceRecoveryPointPage>::success(std::move(page));
    }

    personal_repository::RepositoryCatalogScanner scanner(storage.value()->reader(),
                                                          storage.value()->enumerator(), limits_);
    auto scanned =
        scanner.scan({request.page.continuation_token, request.page.maximum_results}, cancellation);
    if (!scanned) {
        return base::Result<contracts::ServiceRecoveryPointPage>::failure(scanned.error());
    }
    contracts::ServiceRecoveryPointPage page;
    page.repository_connection_id = connection.value()->connection_id;
    page.catalog = map_page(std::move(scanned).value());
    auto valid_page = contracts::validate_service_recovery_point_page(page);
    return valid_page
               ? base::Result<contracts::ServiceRecoveryPointPage>::success(std::move(page))
               : base::Result<contracts::ServiceRecoveryPointPage>::failure(valid_page.error());
}

} // namespace aegra::application
