#pragma once

// Shared Catalog chain resolution + authenticated file_set chain open for Service V4
// ListRecoveryPointEntries / PrepareFileRestore / StartFileRestore / file_set Verify.

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/repository_storage.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::apps::service {

struct OpenedFileRecoveryChain final {
    std::unique_ptr<adapters::personal_archive::PersonalFileArchiveChainReader> reader;
    /// Base-first Catalog entries (Full root … tip).
    std::vector<personal_repository::CatalogEntry> catalog_layers;
    /// Absolute UTF-8 Archive paths, base-first, matching catalog_layers.
    std::vector<std::string> archive_paths_utf8;
    /// Materialized password used to open every layer (empty = unencrypted).
    std::string password;
    std::string tip_index_digest;
    std::string chain_generation_digest;
    std::string repository_locator;
};

/// Resolves tip → Full root Catalog chain (base-first) under an available connection.
[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
resolve_file_set_catalog_chain(ports::IControlPlaneDatabase& control_plane,
                               ports::IRepositoryStorageFactory& storage_factory,
                               std::string_view connection_id, std::string_view recovery_point_id,
                               base::CancellationToken cancellation);

/// Opens and authenticates the complete file_set chain for browse/restore/verify preflight.
/// archive_secret_ref overrides connection default credential when non-empty.
[[nodiscard]] base::Result<OpenedFileRecoveryChain>
open_file_recovery_chain(ports::IControlPlaneDatabase& control_plane,
                         ports::IRepositoryStorageFactory& storage_factory,
                         std::string_view connection_id, std::string_view recovery_point_id,
                         const std::optional<std::string>& archive_secret_ref,
                         base::CancellationToken cancellation);

/// Maps chain open / resolve errors to stable file_recover.* codes where applicable.
[[nodiscard]] base::Error map_file_recover_open_error(const base::Error& error,
                                                      bool credential_supplied);

} // namespace aegra::apps::service
