#include "worker_job_service_restore_shared.h"

#include "worker_job_service_detail.h"

#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/personal_repository/chain_graph.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <optional>
#include <utility>

namespace aegra::apps::service::worker_job_detail {
namespace {

constexpr std::uint64_t kRestorePreflightTtlMs = 5U * 60U * 1'000U;
constexpr std::string_view kDiskSourcePrefix = "disk.";
constexpr std::string_view kVolumeSourcePrefix = "vol.";

[[nodiscard]] std::optional<std::uint32_t>
parse_disk_source_number(const std::string_view source_id) noexcept {
    if (!is_disk_target_id(source_id)) {
        return std::nullopt;
    }
    const auto digits = source_id.substr(kDiskSourcePrefix.size());
    if (digits.empty() || !std::ranges::all_of(digits, [](const unsigned char ch) {
            return std::isdigit(ch) != 0;
        })) {
        return std::nullopt;
    }
    std::uint32_t number = 0;
    const auto* begin = digits.data();
    const auto* end = begin + digits.size();
    if (std::from_chars(begin, end, number).ec != std::errc{}) {
        return std::nullopt;
    }
    return number;
}

[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
load_catalog_entries(ports::IControlPlaneDatabase& control_plane,
                     ports::IRepositoryStorageFactory& storage_factory,
                     const std::string_view connection_id,
                     const base::CancellationToken cancellation) {
    auto repository = control_plane.get_repository_connection(connection_id, cancellation);
    if (!repository) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            repository.error());
    }
    if (!repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    auto storage = storage_factory.open(repository.value()->locator, cancellation);
    if (!storage) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            storage.error());
    }
    personal_repository::RepositoryCatalogScanner scanner(storage.value()->reader(),
                                                          storage.value()->enumerator());
    auto loaded = scanner.load_entries(cancellation);
    if (!loaded) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            loaded.error());
    }
    return base::Result<std::vector<personal_repository::CatalogEntry>>::success(
        std::move(loaded).value().entries);
}

} // namespace

bool is_disk_target_id(const std::string_view source_id) noexcept {
    return source_id.starts_with(kDiskSourcePrefix) && source_id.size() > kDiskSourcePrefix.size();
}

bool is_volume_target_id(const std::string_view source_id) noexcept {
    return source_id.starts_with(kVolumeSourcePrefix) &&
           source_id.size() > kVolumeSourcePrefix.size();
}

base::Result<contracts::SourceInventoryItem>
find_inventory_item(application::ISourceInventoryQuery& inventory, const std::string_view source_id,
                    const base::CancellationToken cancellation) {
    contracts::SourceInventoryListRequest request;
    request.include_unavailable = true;
    request.page.maximum_results = contracts::kMaximumServicePageResults;
    auto page = inventory.list_sources(request, cancellation);
    if (!page) {
        return base::Result<contracts::SourceInventoryItem>::failure(page.error());
    }
    for (const auto& item : page.value().items) {
        if (item.source_id == source_id) {
            return base::Result<contracts::SourceInventoryItem>::success(item);
        }
    }
    if (const auto disk_number = parse_disk_source_number(source_id)) {
        const contracts::SourceInventoryItem* first = nullptr;
        bool is_system = false;
        std::uint64_t disk_capacity = 0;
        for (const auto& item : page.value().items) {
            if (item.disk_number != *disk_number) {
                continue;
            }
            if (first == nullptr) {
                first = &item;
            }
            is_system = is_system || item.is_system;
            const auto item_disk = item.disk_capacity_bytes > 0 ? item.disk_capacity_bytes
                                                                : item.capacity_bytes;
            disk_capacity = (std::max)(disk_capacity, item_disk);
        }
        if (first != nullptr) {
            contracts::SourceInventoryItem synthetic = *first;
            synthetic.source_id = std::string(source_id);
            synthetic.display_name = "Disk " + std::to_string(*disk_number);
            synthetic.is_system = is_system;
            synthetic.disk_capacity_bytes = disk_capacity;
            return base::Result<contracts::SourceInventoryItem>::success(std::move(synthetic));
        }
    }
    return base::Result<contracts::SourceInventoryItem>::failure(
        {base::ErrorCode::kNotFound, "restore target was not found in inventory"});
}

std::string make_volume_restore_fingerprint(const VolumeRestoreChain& chain) {
    std::string out = "volc|" + std::to_string(chain.source_volume_index) + "|" +
                      std::to_string(chain.volume_size_bytes) + "|" +
                      std::to_string(chain.layers.size());
    for (const auto& layer : chain.layers) {
        out.push_back('|');
        out.append(layer.archive_key);
        out.push_back('|');
        out.append(layer.file_uuid);
    }
    return out;
}

base::Result<std::vector<personal_repository::CatalogEntry>>
resolve_restore_chain_entries(ports::IControlPlaneDatabase& control_plane,
                              ports::IRepositoryStorageFactory& storage_factory,
                              const std::string_view connection_id,
                              const std::string_view recovery_point_id,
                              const base::CancellationToken cancellation) {
    auto entries =
        load_catalog_entries(control_plane, storage_factory, connection_id, cancellation);
    if (!entries) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            entries.error());
    }
    auto graph = personal_repository::RecoveryPointGraph::build(std::move(entries).value());
    if (!graph) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            graph.error());
    }
    auto chain = graph.value().resolve_chain(recovery_point_id);
    if (!chain) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(chain.error());
    }
    return base::Result<std::vector<personal_repository::CatalogEntry>>::success(
        std::move(chain).value());
}

base::Result<std::vector<personal_repository::CatalogEntry>>
load_restore_chain_or_fail(ports::IControlPlaneDatabase& control_plane,
                           ports::IRepositoryStorageFactory& storage_factory,
                           const std::string_view connection_id,
                           const std::string_view recovery_point_id,
                           const base::CancellationToken cancellation) {
    auto chain_entries = resolve_restore_chain_entries(control_plane, storage_factory, connection_id,
                                                       recovery_point_id, cancellation);
    if (!chain_entries) {
        return chain_entries;
    }
    if (chain_entries.value().empty() ||
        chain_entries.value().back().file_uuid != recovery_point_id) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            {base::ErrorCode::kConflict, "recovery point chain is incomplete"});
    }
    return chain_entries;
}

base::Result<contracts::RestorePreflight>
persist_restore_preflight(ports::IControlPlaneDatabase& control_plane, ports::IClock& clock,
                          ports::IRandomSource& random,
                          const contracts::RestorePreflightRequest& request,
                          PreparedRestoreChain prepared, const base::CancellationToken cancellation) {
    const auto now = clock.now_utc_ms();
    if (now < 0) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kInternal, "restore preflight clock is invalid"});
    }
    const auto now_u = static_cast<std::uint64_t>(now);
    if (now_u > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) -
                     kRestorePreflightTtlMs) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kInternal, "restore preflight clock is invalid"});
    }
    auto token = random_id("preflight-", random, cancellation);
    if (!token) {
        return base::Result<contracts::RestorePreflight>::failure(token.error());
    }
    ports::RestorePreflightRecord record;
    record.preflight_token = token.value();
    record.repository_connection_id = request.repository_connection_id;
    record.repository_uuid = std::move(prepared.repository_uuid);
    record.recovery_point_id = request.recovery_point_id;
    record.target_source_id = request.target_source_id;
    record.chain_fingerprint = std::move(prepared.chain_fingerprint);
    record.logical_size_bytes = prepared.logical_size_bytes;
    record.target_capacity_bytes = prepared.target_capacity_bytes;
    record.chain_depth = prepared.chain_depth;
    record.created_utc_ms = now_u;
    record.expires_utc_ms = now_u + kRestorePreflightTtlMs;
    record.volume_size_policy = prepared.volume_size_policy;
    record.feasibility = prepared.feasibility;
    record.minimum_target_bytes = prepared.minimum_target_bytes;
    record.relocation_bytes = prepared.relocation_bytes;
    record.scratch_upper_bound_bytes = prepared.scratch_upper_bound_bytes;
    record.shrink_plan_digest = prepared.shrink_plan_digest;
    record.target_binding_digest = std::move(prepared.target_binding_digest);
    auto unit = control_plane.begin_unit_of_work(cancellation);
    if (!unit) {
        return base::Result<contracts::RestorePreflight>::failure(unit.error());
    }
    if (auto inserted = unit.value()->restore_preflights().insert(record, cancellation);
        !inserted) {
        unit.value()->rollback();
        return base::Result<contracts::RestorePreflight>::failure(inserted.error());
    }
    if (auto committed = unit.value()->commit(cancellation); !committed) {
        return base::Result<contracts::RestorePreflight>::failure(committed.error());
    }
    contracts::RestorePreflight preflight;
    preflight.preflight_token = record.preflight_token;
    preflight.repository_connection_id = record.repository_connection_id;
    preflight.recovery_point_id = record.recovery_point_id;
    preflight.target_source_id = record.target_source_id;
    preflight.logical_size_bytes = record.logical_size_bytes;
    preflight.target_capacity_bytes = record.target_capacity_bytes;
    preflight.chain_depth = record.chain_depth;
    preflight.expires_utc_ms = record.expires_utc_ms;
    preflight.volume_size_policy = record.volume_size_policy;
    preflight.feasibility = record.feasibility;
    preflight.restore_eligible =
        record.feasibility == contracts::RestoreFeasibility::kEligible;
    preflight.minimum_target_bytes = record.minimum_target_bytes;
    preflight.relocation_bytes = record.relocation_bytes;
    preflight.scratch_upper_bound_bytes = record.scratch_upper_bound_bytes;
    preflight.shrink_plan_digest = record.shrink_plan_digest;
    preflight.restriction_codes = std::move(prepared.restriction_codes);
    preflight.warning_codes = std::move(prepared.warning_codes);
    preflight.message_code = std::move(prepared.message_code);
    return base::Result<contracts::RestorePreflight>::success(std::move(preflight));
}

} // namespace aegra::apps::service::worker_job_detail
