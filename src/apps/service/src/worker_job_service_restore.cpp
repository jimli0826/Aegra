#include "aegra/apps/service/worker_job_service.h"

#include "worker_job_service_detail.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/application/source_inventory_query.h"
#include "aegra/apps/service/worker_supervisor.h"
#include "aegra/format/manifest.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/personal_repository/chain_graph.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/random.h"
#include "aegra/ports/repository_storage.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

using worker_job_detail::acknowledgement;
using worker_job_detail::path_from_utf8;
using worker_job_detail::random_id;
using worker_job_detail::resolve_archive_absolute_path;

constexpr std::uint64_t kRestorePreflightTtlMs = 5U * 60U * 1'000U;
constexpr std::string_view kDiskSourcePrefix = "disk.";
constexpr std::string_view kVolumeSourcePrefix = "vol.";

[[nodiscard]] bool is_disk_target_id(const std::string_view source_id) noexcept {
    return source_id.starts_with(kDiskSourcePrefix) && source_id.size() > kDiskSourcePrefix.size();
}

[[nodiscard]] bool is_volume_target_id(const std::string_view source_id) noexcept {
    return source_id.starts_with(kVolumeSourcePrefix) &&
           source_id.size() > kVolumeSourcePrefix.size();
}

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

[[nodiscard]] base::Result<contracts::SourceInventoryItem>
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
    // Fallback: older inventory builds only emitted disk.N for empty disks. Resolve disk.N by
    // matching disk_number on any volume row so restore targets remain addressable.
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

struct RestoreChainLayer final {
    std::string archive_key;
    std::string file_uuid;
};

struct DiskRestoreChain final {
    std::uint32_t source_disk_number{0};
    std::uint64_t disk_size_bytes{0};
    /// Base-first Full → … → tip.
    std::vector<RestoreChainLayer> layers;
};

struct VolumeRestoreChain final {
    std::uint32_t source_volume_index{0};
    std::uint64_t volume_size_bytes{0};
    /// Base-first Full → … → tip.
    std::vector<RestoreChainLayer> layers;
};

[[nodiscard]] std::vector<std::string_view> split_fingerprint_parts(const std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto bar = text.find('|', start);
        if (bar == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, bar - start));
        start = bar + 1;
    }
    return parts;
}

[[nodiscard]] base::Result<std::vector<RestoreChainLayer>>
parse_restore_chain_layers(const std::vector<std::string_view>& parts, const std::size_t depth_index) {
    if (parts.size() <= depth_index) {
        return base::Result<std::vector<RestoreChainLayer>>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    std::uint32_t depth = 0;
    {
        const auto* begin = parts[depth_index].data();
        const auto* end = begin + parts[depth_index].size();
        if (std::from_chars(begin, end, depth).ec != std::errc{} || depth == 0) {
            return base::Result<std::vector<RestoreChainLayer>>::failure(
                {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
        }
    }
    if (parts.size() != depth_index + 1U + static_cast<std::size_t>(depth) * 2U) {
        return base::Result<std::vector<RestoreChainLayer>>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    std::vector<RestoreChainLayer> layers;
    layers.reserve(depth);
    for (std::uint32_t index = 0; index < depth; ++index) {
        const auto& key = parts[depth_index + 1U + static_cast<std::size_t>(index) * 2U];
        const auto& uuid = parts[depth_index + 2U + static_cast<std::size_t>(index) * 2U];
        if (key.empty() || uuid.empty()) {
            return base::Result<std::vector<RestoreChainLayer>>::failure(
                {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
        }
        layers.push_back({std::string(key), std::string(uuid)});
    }
    return base::Result<std::vector<RestoreChainLayer>>::success(std::move(layers));
}

// Durable fingerprint (base-first):
// diskc|{source_disk}|{disk_size}|{depth}|{key0}|{uuid0}|…|{keyN-1}|{uuidN-1}
[[nodiscard]] std::string make_disk_restore_fingerprint(const DiskRestoreChain& chain) {
    std::string out = "diskc|" + std::to_string(chain.source_disk_number) + "|" +
                      std::to_string(chain.disk_size_bytes) + "|" +
                      std::to_string(chain.layers.size());
    for (const auto& layer : chain.layers) {
        out.push_back('|');
        out.append(layer.archive_key);
        out.push_back('|');
        out.append(layer.file_uuid);
    }
    return out;
}

// volc|{source_volume_index}|{volume_size}|{depth}|{key0}|{uuid0}|…
[[nodiscard]] std::string make_volume_restore_fingerprint(const VolumeRestoreChain& chain) {
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

[[nodiscard]] base::Result<DiskRestoreChain>
parse_disk_restore_fingerprint(const std::string_view fingerprint) {
    if (!fingerprint.starts_with("diskc|")) {
        return base::Result<DiskRestoreChain>::failure(
            {base::ErrorCode::kConflict, "restore preflight is not a disk restore"});
    }
    const auto parts = split_fingerprint_parts(fingerprint);
    if (parts.size() < 4 || parts[0] != "diskc") {
        return base::Result<DiskRestoreChain>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    DiskRestoreChain parsed;
    {
        std::uint32_t source_disk = 0;
        const auto* begin = parts[1].data();
        const auto* end = begin + parts[1].size();
        if (std::from_chars(begin, end, source_disk).ec != std::errc{}) {
            return base::Result<DiskRestoreChain>::failure(
                {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
        }
        parsed.source_disk_number = source_disk;
    }
    {
        std::uint64_t disk_size = 0;
        const auto* begin = parts[2].data();
        const auto* end = begin + parts[2].size();
        if (std::from_chars(begin, end, disk_size).ec != std::errc{} || disk_size == 0) {
            return base::Result<DiskRestoreChain>::failure(
                {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
        }
        parsed.disk_size_bytes = disk_size;
    }
    auto layers = parse_restore_chain_layers(parts, 3);
    if (!layers) {
        return base::Result<DiskRestoreChain>::failure(layers.error());
    }
    parsed.layers = std::move(layers).value();
    return base::Result<DiskRestoreChain>::success(std::move(parsed));
}

[[nodiscard]] base::Result<VolumeRestoreChain>
parse_volume_restore_fingerprint(const std::string_view fingerprint) {
    if (!fingerprint.starts_with("volc|")) {
        return base::Result<VolumeRestoreChain>::failure(
            {base::ErrorCode::kConflict, "restore preflight is not a volume restore"});
    }
    const auto parts = split_fingerprint_parts(fingerprint);
    if (parts.size() < 4 || parts[0] != "volc") {
        return base::Result<VolumeRestoreChain>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    VolumeRestoreChain parsed;
    {
        std::uint32_t volume_index = 0;
        const auto* begin = parts[1].data();
        const auto* end = begin + parts[1].size();
        if (std::from_chars(begin, end, volume_index).ec != std::errc{}) {
            return base::Result<VolumeRestoreChain>::failure(
                {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
        }
        parsed.source_volume_index = volume_index;
    }
    {
        std::uint64_t volume_size = 0;
        const auto* begin = parts[2].data();
        const auto* end = begin + parts[2].size();
        if (std::from_chars(begin, end, volume_size).ec != std::errc{} || volume_size == 0) {
            return base::Result<VolumeRestoreChain>::failure(
                {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
        }
        parsed.volume_size_bytes = volume_size;
    }
    auto layers = parse_restore_chain_layers(parts, 3);
    if (!layers) {
        return base::Result<VolumeRestoreChain>::failure(layers.error());
    }
    parsed.layers = std::move(layers).value();
    return base::Result<VolumeRestoreChain>::success(std::move(parsed));
}

[[nodiscard]] base::Result<std::uint64_t>
source_disk_size_from_archive(const std::string& archive_path_utf8,
                              const std::uint32_t source_disk_number,
                              const std::string& password) {
    auto path = path_from_utf8(archive_path_utf8);
    if (!path) {
        return base::Result<std::uint64_t>::failure(path.error());
    }
    adapters::personal_archive::ArchiveOpenRequest open_request;
    open_request.source = std::move(path).value();
    open_request.password = password;
    auto reader = adapters::personal_archive::PersonalArchiveReader::open(open_request);
    if (!reader) {
        return base::Result<std::uint64_t>::failure(reader.error());
    }
    for (const auto& disk : reader.value()->manifest().disks) {
        if (disk.disk_number == source_disk_number) {
            if (disk.disk_size == 0) {
                return base::Result<std::uint64_t>::failure(
                    {base::ErrorCode::kConflict, "source disk size is unavailable"});
            }
            return base::Result<std::uint64_t>::success(disk.disk_size);
        }
    }
    return base::Result<std::uint64_t>::failure(
        {base::ErrorCode::kNotFound, "source disk is not present in archive manifest"});
}

[[nodiscard]] base::Result<std::uint64_t>
source_volume_size_from_archive(const std::string& archive_path_utf8,
                                const std::uint32_t source_volume_index,
                                const std::string& password) {
    auto path = path_from_utf8(archive_path_utf8);
    if (!path) {
        return base::Result<std::uint64_t>::failure(path.error());
    }
    adapters::personal_archive::ArchiveOpenRequest open_request;
    open_request.source = std::move(path).value();
    open_request.password = password;
    auto reader = adapters::personal_archive::PersonalArchiveReader::open(open_request);
    if (!reader) {
        return base::Result<std::uint64_t>::failure(reader.error());
    }
    for (const auto& volume : reader.value()->manifest().volumes) {
        if (volume.volume_index == source_volume_index) {
            if (volume.total_size == 0) {
                return base::Result<std::uint64_t>::failure(
                    {base::ErrorCode::kConflict, "source volume size is unavailable"});
            }
            return base::Result<std::uint64_t>::success(volume.total_size);
        }
    }
    return base::Result<std::uint64_t>::failure(
        {base::ErrorCode::kNotFound, "source volume is not present in archive manifest"});
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

[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
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

[[nodiscard]] std::string restore_request_fingerprint(const std::string_view preflight_token) {
    return "start-restore|" + std::string(preflight_token);
}

struct PreparedRestoreChain final {
    std::string repository_uuid;
    std::string chain_fingerprint;
    std::uint64_t logical_size_bytes{0};
    std::uint64_t target_capacity_bytes{0};
    std::uint32_t chain_depth{0};
};

[[nodiscard]] base::Result<contracts::RestorePreflight>
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
    preflight.restore_eligible = true;
    preflight.message_code = "restore.preflight_ready";
    return base::Result<contracts::RestorePreflight>::success(std::move(preflight));
}

[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
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

} // namespace

base::Result<contracts::RestorePreflight>
WorkerJobService::prepare_restore(const contracts::RestorePreflightRequest& request,
                                  const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_restore_preflight_request(request); !valid) {
        return base::Result<contracts::RestorePreflight>::failure(valid.error());
    }
    const bool disk_target = is_disk_target_id(request.target_source_id);
    const bool volume_target = is_volume_target_id(request.target_source_id);
    if (!disk_target && !volume_target) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kInvalidArgument,
             "restore target must be disk.N or a volume source id"});
    }
    auto target = find_inventory_item(source_inventory_, request.target_source_id, cancellation);
    if (!target) {
        return base::Result<contracts::RestorePreflight>::failure(target.error());
    }
    if (target.value().is_system) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kConflict,
             disk_target ? "system disk restore requires PE (not available online)"
                         : "system volume restore requires PE (not available online)"});
    }
    if (volume_target && target.value().is_read_only) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kConflict, "restore target volume is read-only"});
    }
    if (target.value().availability != contracts::SourceAvailability::kAvailable) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kConflict, "restore target is unavailable"});
    }
    const auto target_capacity =
        disk_target
            ? (target.value().disk_capacity_bytes > 0 ? target.value().disk_capacity_bytes
                                                      : target.value().capacity_bytes)
            : target.value().capacity_bytes;
    if (target_capacity == 0) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kConflict, "restore target capacity is unavailable"});
    }
    auto chain_entries =
        load_restore_chain_or_fail(control_plane_, storage_factory_, request.repository_connection_id,
                                   request.recovery_point_id, cancellation);
    if (!chain_entries) {
        return base::Result<contracts::RestorePreflight>::failure(chain_entries.error());
    }
    auto repository =
        control_plane_.get_repository_connection(request.repository_connection_id, cancellation);
    if (!repository || !repository.value()) {
        return base::Result<contracts::RestorePreflight>::failure(
            !repository ? repository.error()
                        : base::Error{base::ErrorCode::kNotFound, "repository connection not found"});
    }
    auto tip_path = resolve_archive_absolute_path(repository.value()->locator,
                                                  chain_entries.value().back().archive_main_key);
    if (!tip_path) {
        return base::Result<contracts::RestorePreflight>::failure(tip_path.error());
    }
    PreparedRestoreChain prepared;
    prepared.repository_uuid = chain_entries.value().back().repository_uuid;
    prepared.chain_depth = static_cast<std::uint32_t>(chain_entries.value().size());
    prepared.target_capacity_bytes = target_capacity;
    if (disk_target) {
        auto disk_size = source_disk_size_from_archive(tip_path.value(), request.source_disk_number,
                                                       request.archive_password);
        if (!disk_size) {
            return base::Result<contracts::RestorePreflight>::failure(disk_size.error());
        }
        if (target_capacity < disk_size.value()) {
            return base::Result<contracts::RestorePreflight>::failure(
                {base::ErrorCode::kConflict, "restore target is smaller than the source disk"});
        }
        DiskRestoreChain chain;
        chain.source_disk_number = request.source_disk_number;
        chain.disk_size_bytes = disk_size.value();
        chain.layers.reserve(chain_entries.value().size());
        for (const auto& entry : chain_entries.value()) {
            chain.layers.push_back({entry.archive_main_key, entry.file_uuid});
        }
        prepared.logical_size_bytes = disk_size.value();
        prepared.chain_fingerprint = make_disk_restore_fingerprint(chain);
    } else {
        auto volume_size = source_volume_size_from_archive(
            tip_path.value(), request.source_volume_index, request.archive_password);
        if (!volume_size) {
            return base::Result<contracts::RestorePreflight>::failure(volume_size.error());
        }
        if (target_capacity < volume_size.value()) {
            return base::Result<contracts::RestorePreflight>::failure(
                {base::ErrorCode::kConflict, "restore target is smaller than the source volume"});
        }
        VolumeRestoreChain chain;
        chain.source_volume_index = request.source_volume_index;
        chain.volume_size_bytes = volume_size.value();
        chain.layers.reserve(chain_entries.value().size());
        for (const auto& entry : chain_entries.value()) {
            chain.layers.push_back({entry.archive_main_key, entry.file_uuid});
        }
        prepared.logical_size_bytes = volume_size.value();
        prepared.chain_fingerprint = make_volume_restore_fingerprint(chain);
    }
    return persist_restore_preflight(control_plane_, clock_, random_, request, std::move(prepared),
                                     cancellation);
}

base::Result<contracts::CommandAcknowledgement>
WorkerJobService::start_restore(const contracts::StartRestoreCommand& command,
                                const std::string_view idempotency_key,
                                const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_start_restore_command(command); !valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    if (idempotency_key.empty()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kInvalidArgument, "idempotency key is required"});
    }
    auto existing = control_plane_.get_job_by_idempotency_key(idempotency_key, cancellation);
    if (!existing) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
    }
    const auto fingerprint = restore_request_fingerprint(command.preflight_token);
    if (existing.value()) {
        if (existing.value()->operation != contracts::JobOperation::kRestore ||
            existing.value()->request_fingerprint != fingerprint) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "idempotency key request mismatch"});
        }
        return base::Result<contracts::CommandAcknowledgement>::success(
            acknowledgement(existing.value()->job_id, contracts::CommandDisposition::kReplayed,
                            existing.value()->job_id));
    }
    auto preflight =
        control_plane_.get_restore_preflight(command.preflight_token, cancellation);
    if (!preflight) {
        return base::Result<contracts::CommandAcknowledgement>::failure(preflight.error());
    }
    if (!preflight.value()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kNotFound, "restore preflight was not found"});
    }
    const auto& record = *preflight.value();
    const auto now = static_cast<std::uint64_t>((std::max)(clock_.now_utc_ms(), 0LL));
    if (now >= record.expires_utc_ms) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "restore preflight has expired"});
    }
    const bool disk_mode = record.chain_fingerprint.starts_with("diskc|");
    const bool volume_mode = record.chain_fingerprint.starts_with("volc|");
    if (!disk_mode && !volume_mode) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    DiskRestoreChain disk_chain;
    VolumeRestoreChain volume_chain;
    const std::vector<RestoreChainLayer>* layers = nullptr;
    std::uint64_t expected_size = 0;
    if (disk_mode) {
        auto parsed = parse_disk_restore_fingerprint(record.chain_fingerprint);
        if (!parsed) {
            return base::Result<contracts::CommandAcknowledgement>::failure(parsed.error());
        }
        disk_chain = std::move(parsed).value();
        layers = &disk_chain.layers;
        expected_size = disk_chain.disk_size_bytes;
    } else {
        auto parsed = parse_volume_restore_fingerprint(record.chain_fingerprint);
        if (!parsed) {
            return base::Result<contracts::CommandAcknowledgement>::failure(parsed.error());
        }
        volume_chain = std::move(parsed).value();
        layers = &volume_chain.layers;
        expected_size = volume_chain.volume_size_bytes;
    }
    if (layers->empty() || layers->back().file_uuid != record.recovery_point_id) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    auto by_token = control_plane_.get_job_by_preflight_token(command.preflight_token, cancellation);
    if (!by_token) {
        return base::Result<contracts::CommandAcknowledgement>::failure(by_token.error());
    }
    if (by_token.value()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "restore preflight already started a job"});
    }
    auto target = find_inventory_item(source_inventory_, record.target_source_id, cancellation);
    if (!target) {
        return base::Result<contracts::CommandAcknowledgement>::failure(target.error());
    }
    if (target.value().is_system) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict,
             disk_mode ? "system disk restore requires PE (not available online)"
                       : "system volume restore requires PE (not available online)"});
    }
    if (volume_mode && target.value().is_read_only) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "restore target volume is read-only"});
    }
    const auto target_capacity =
        disk_mode ? (target.value().disk_capacity_bytes > 0 ? target.value().disk_capacity_bytes
                                                            : target.value().capacity_bytes)
                  : target.value().capacity_bytes;
    if (target_capacity < expected_size) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict,
             disk_mode ? "restore target is smaller than the source disk"
                       : "restore target is smaller than the source volume"});
    }
    auto repository =
        control_plane_.get_repository_connection(record.repository_connection_id, cancellation);
    if (!repository || !repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    auto live_chain = resolve_restore_chain_entries(
        control_plane_, storage_factory_, record.repository_connection_id, record.recovery_point_id,
        cancellation);
    if (!live_chain) {
        return base::Result<contracts::CommandAcknowledgement>::failure(live_chain.error());
    }
    if (live_chain.value().size() != layers->size()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "restore chain changed after preflight"});
    }
    for (std::size_t index = 0; index < live_chain.value().size(); ++index) {
        if (live_chain.value()[index].file_uuid != (*layers)[index].file_uuid ||
            live_chain.value()[index].archive_main_key != (*layers)[index].archive_key) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "restore chain changed after preflight"});
        }
    }
    std::vector<std::string> source_refs;
    source_refs.reserve(layers->size());
    for (const auto& layer : *layers) {
        auto archive_path =
            resolve_archive_absolute_path(repository.value()->locator, layer.archive_key);
        if (!archive_path) {
            return base::Result<contracts::CommandAcknowledgement>::failure(archive_path.error());
        }
        source_refs.push_back(std::move(archive_path).value());
    }
    if (disk_mode) {
        auto disk_size = source_disk_size_from_archive(
            source_refs.back(), disk_chain.source_disk_number, command.archive_password);
        if (!disk_size) {
            return base::Result<contracts::CommandAcknowledgement>::failure(disk_size.error());
        }
        if (disk_size.value() != expected_size) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "restore source disk size changed after preflight"});
        }
    } else {
        auto volume_size = source_volume_size_from_archive(
            source_refs.back(), volume_chain.source_volume_index, command.archive_password);
        if (!volume_size) {
            return base::Result<contracts::CommandAcknowledgement>::failure(volume_size.error());
        }
        if (volume_size.value() != expected_size) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict,
                 "restore source volume size changed after preflight"});
        }
    }
    auto job_id = random_id("job-", random_, cancellation);
    auto trace_id = random_id("trace-", random_, cancellation);
    if (!job_id || !trace_id) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            !job_id ? job_id.error() : trace_id.error());
    }
    contracts::JobRequest worker;
    worker.job_id = job_id.value();
    worker.tenant_id = "personal";
    worker.operation = contracts::JobOperation::kRestore;
    worker.source_refs = std::move(source_refs);
    if (disk_mode) {
        worker.target_ref =
            std::string(R"(\\.\PhysicalDrive)") + std::to_string(target.value().disk_number);
    } else {
        // stable_key is the trusted Volume GUID path (never sent to Desktop).
        auto resolved =
            source_inventory_.resolve_source(record.target_source_id, cancellation);
        if (!resolved) {
            return base::Result<contracts::CommandAcknowledgement>::failure(resolved.error());
        }
        worker.target_ref = std::move(resolved).value().stable_key;
    }
    if (command.archive_password.empty()) {
        worker.credential_refs.assign(worker.source_refs.size(), contracts::SecretRef{});
    } else {
        auto protected_secret = adapters::windows_system::protect_local_machine_secret(
            command.archive_password, job_id.value());
        if (!protected_secret) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                protected_secret.error());
        }
        const contracts::SecretRef layer_secret = std::move(protected_secret).value();
        worker.credential_refs.assign(worker.source_refs.size(), layer_secret);
    }
    worker.trace_id = trace_id.value();
    contracts::RestoreOptions restore;
    if (disk_mode) {
        restore.disk_restore = true;
        restore.source_disk_number = disk_chain.source_disk_number;
        restore.bring_target_online = true;
        restore.preserve_disk_signature = command.preserve_disk_signature;
        restore.auto_expand_last_partition = command.auto_expand_last_partition;
    } else {
        restore.disk_restore = false;
        restore.source_volume_index = volume_chain.source_volume_index;
    }
    worker.restore = std::move(restore);

    WorkerJobRequest request;
    request.worker_request = std::move(worker);
    request.source_ids = {record.recovery_point_id};
    request.repository_connection_id = record.repository_connection_id;
    request.idempotency_key = std::string(idempotency_key);
    request.request_fingerprint = fingerprint;
    request.preflight_token = command.preflight_token;
    request.target_source_id = record.target_source_id;
    auto submitted = supervisor_.submit(request, cancellation);
    if (!submitted) {
        return base::Result<contracts::CommandAcknowledgement>::failure(submitted.error());
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        acknowledgement(job_id.value(), contracts::CommandDisposition::kAccepted, job_id.value()));
}

} // namespace aegra::apps::service
