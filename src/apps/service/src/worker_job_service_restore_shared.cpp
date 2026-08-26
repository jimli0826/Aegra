#include "worker_job_service_restore_shared.h"

#include "worker_job_service_detail.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/format/manifest.h"
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

namespace {

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
parse_restore_chain_layers(const std::vector<std::string_view>& parts,
                           const std::size_t depth_index) {
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

template <typename Value>
[[nodiscard]] base::Result<Value> parse_fingerprint_number(const std::string_view text,
                                                           const bool reject_zero) {
    Value value{};
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    if (std::from_chars(begin, end, value).ec != std::errc{} || (reject_zero && value == 0)) {
        return base::Result<Value>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    return base::Result<Value>::success(value);
}

} // namespace

std::string make_disk_restore_fingerprint(const DiskRestoreChain& chain) {
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

base::Result<DiskRestoreChain> parse_disk_restore_fingerprint(const std::string_view fingerprint) {
    if (!fingerprint.starts_with("diskc|")) {
        return base::Result<DiskRestoreChain>::failure(
            {base::ErrorCode::kConflict, "restore preflight is not a disk restore"});
    }
    const auto parts = split_fingerprint_parts(fingerprint);
    if (parts.size() < 4 || parts[0] != "diskc") {
        return base::Result<DiskRestoreChain>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    auto source_disk = parse_fingerprint_number<std::uint32_t>(parts[1], false);
    auto disk_size = parse_fingerprint_number<std::uint64_t>(parts[2], true);
    if (!source_disk || !disk_size) {
        return base::Result<DiskRestoreChain>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    auto layers = parse_restore_chain_layers(parts, 3);
    if (!layers) {
        return base::Result<DiskRestoreChain>::failure(layers.error());
    }
    DiskRestoreChain parsed;
    parsed.source_disk_number = source_disk.value();
    parsed.disk_size_bytes = disk_size.value();
    parsed.layers = std::move(layers).value();
    return base::Result<DiskRestoreChain>::success(std::move(parsed));
}

base::Result<VolumeRestoreChain>
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
    auto volume_index = parse_fingerprint_number<std::uint32_t>(parts[1], false);
    auto volume_size = parse_fingerprint_number<std::uint64_t>(parts[2], true);
    if (!volume_index || !volume_size) {
        return base::Result<VolumeRestoreChain>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    auto layers = parse_restore_chain_layers(parts, 3);
    if (!layers) {
        return base::Result<VolumeRestoreChain>::failure(layers.error());
    }
    VolumeRestoreChain parsed;
    parsed.source_volume_index = volume_index.value();
    parsed.volume_size_bytes = volume_size.value();
    parsed.layers = std::move(layers).value();
    return base::Result<VolumeRestoreChain>::success(std::move(parsed));
}

base::Result<std::uint64_t>
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

} // namespace aegra::apps::service::worker_job_detail
