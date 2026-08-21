#pragma once

// Shared volume-restore helpers for prepare/analyze/start (Service composition root only).

#include "aegra/application/source_inventory_query.h"
#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/random.h"
#include "aegra/ports/repository_storage.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::apps::service::worker_job_detail {

[[nodiscard]] bool is_disk_target_id(std::string_view source_id) noexcept;
[[nodiscard]] bool is_volume_target_id(std::string_view source_id) noexcept;

[[nodiscard]] base::Result<contracts::SourceInventoryItem>
find_inventory_item(application::ISourceInventoryQuery& inventory, std::string_view source_id,
                    base::CancellationToken cancellation);

struct RestoreChainLayer final {
    std::string archive_key;
    std::string file_uuid;
};

struct VolumeRestoreChain final {
    std::uint32_t source_volume_index{0};
    std::uint64_t volume_size_bytes{0};
    /// Base-first Full → … → tip.
    std::vector<RestoreChainLayer> layers;
};

[[nodiscard]] std::string make_volume_restore_fingerprint(const VolumeRestoreChain& chain);

[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
resolve_restore_chain_entries(ports::IControlPlaneDatabase& control_plane,
                              ports::IRepositoryStorageFactory& storage_factory,
                              std::string_view connection_id, std::string_view recovery_point_id,
                              base::CancellationToken cancellation);

[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
load_restore_chain_or_fail(ports::IControlPlaneDatabase& control_plane,
                           ports::IRepositoryStorageFactory& storage_factory,
                           std::string_view connection_id, std::string_view recovery_point_id,
                           base::CancellationToken cancellation);

struct PreparedRestoreChain final {
    std::string repository_uuid;
    std::string chain_fingerprint;
    std::uint64_t logical_size_bytes{0};
    std::uint64_t target_capacity_bytes{0};
    std::uint32_t chain_depth{0};
    contracts::VolumeSizePolicy volume_size_policy{contracts::VolumeSizePolicy::kRequireSourceSize};
    contracts::RestoreFeasibility feasibility{contracts::RestoreFeasibility::kEligible};
    std::uint64_t minimum_target_bytes{0};
    std::uint64_t relocation_bytes{0};
    std::uint64_t scratch_upper_bound_bytes{0};
    std::string shrink_plan_digest;
    std::string target_binding_digest;
    std::vector<std::string> restriction_codes;
    std::vector<std::string> warning_codes;
    std::string message_code{"restore.preflight_ready"};
};

[[nodiscard]] base::Result<contracts::RestorePreflight>
persist_restore_preflight(ports::IControlPlaneDatabase& control_plane, ports::IClock& clock,
                          ports::IRandomSource& random,
                          const contracts::RestorePreflightRequest& request,
                          PreparedRestoreChain prepared, base::CancellationToken cancellation);

} // namespace aegra::apps::service::worker_job_detail
