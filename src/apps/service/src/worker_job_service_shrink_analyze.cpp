#include "aegra/apps/service/worker_job_service.h"

#include "aegra/apps/service/service_host.h"

#include "service_log_formatter.h"
#include "worker_job_service_detail.h"
#include "worker_job_service_restore_shared.h"

#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/ntfs_resize/ntfs_shrink_analyzer.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/repository_storage.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

using worker_job_detail::find_inventory_item;
using worker_job_detail::is_disk_target_id;
using worker_job_detail::is_volume_target_id;
using worker_job_detail::load_restore_chain_or_fail;
using worker_job_detail::make_volume_restore_fingerprint;
using worker_job_detail::path_from_utf8;
using worker_job_detail::persist_restore_preflight;
using worker_job_detail::PreparedRestoreChain;
using worker_job_detail::resolve_archive_absolute_path;
using worker_job_detail::VolumeRestoreChain;

class ShrinkAnalyzeFlowLog final : public ntfs_resize::INtfsShrinkAnalysisObserver {
  public:
    ShrinkAnalyzeFlowLog(IServiceLog* const logger,
                         const contracts::RestorePreflightRequest& request) noexcept
        : flow_(logger), recovery_point_id_(request.recovery_point_id),
          source_volume_index_(request.source_volume_index),
          target_source_id_(request.target_source_id) {}

    void started() noexcept {
        flow_.section("AnalyzeNtfsShrink");
        flow_.field("recovery_point_id", recovery_point_id_);
        flow_.field_u64("source_volume_index", source_volume_index_);
        flow_.field("target_source_id", target_source_id_);
        begin_timed_stage("prepare_inputs");
    }

    void inputs_ready(const std::uint64_t source_size_bytes, const std::size_t chain_depth,
                      const ports::BlockDeviceGeometry& geometry) noexcept {
        flow_.field_bytes("source_logical_size", source_size_bytes);
        flow_.field_u64("chain_depth", chain_depth);
        flow_.field_bytes("target_capacity", geometry.capacity_bytes);
        flow_.field_u64("target_logical_sector_bytes", geometry.logical_sector_size);
        flow_.field_u64("target_physical_sector_bytes", geometry.physical_sector_size);
        end_timed_stage_ok();
        begin_timed_stage("analyze_candidates");
    }

    void begin_candidate(const std::string_view phase) noexcept {
        active_phase_ = phase;
        active_stage_ = {};
        counts_.reset();
        allocation_.reset();
    }

    void search_bounds(const std::uint64_t, const std::uint64_t, const std::uint64_t,
                       const std::uint64_t) noexcept {
        ++minimum_search_steps_;
    }

    void candidate_result(const std::string_view phase, const std::uint64_t capacity_bytes,
                          const base::Error* const error) noexcept {
        if (phase == "minimum_search") {
            return;
        }
        flow_.section(std::string("Candidate: ") + std::string(phase));
        flow_.field_bytes("candidate_capacity", capacity_bytes);
        if (counts_) {
            flow_.field_u64("new_total_clusters", counts_->new_total_cluster_count);
        }
        if (allocation_) {
            flow_.field_u64("allocated_beyond_clusters",
                            allocation_->allocated_beyond_cluster_count);
            flow_.field_u64("allocatable_clusters", allocation_->allocatable_cluster_count);
            flow_.field_u64("reserved_clusters", allocation_->reserved_cluster_count);
        }
        flow_.field("result", error == nullptr ? "accepted" : "rejected");
        if (error != nullptr) {
            if (!active_stage_.empty()) {
                flow_.field("failure_stage", active_stage_);
            }
            flow_.field("error_code", base::error_code_name(error->code));
            flow_.field("message_code", error->message);
        }
    }

    void failed(const std::string_view stage, const base::Error& error) noexcept {
        end_timed_stage_fail(base::error_code_name(error.code), error.message);
        flow_.section("Result");
        flow_.field("outcome", "failed");
        flow_.field("stage", stage);
        flow_.field("error_code", base::error_code_name(error.code));
        flow_.field("message_code", error.message);
        if (!active_phase_.empty()) {
            flow_.field("candidate_phase", active_phase_);
        }
        if (!active_stage_.empty()) {
            flow_.field("candidate_stage", active_stage_);
        }
    }

    void completed(const contracts::RestorePreflight& preflight) noexcept {
        end_timed_stage_ok();
        flow_.section("Result");
        flow_.field("outcome", "completed");
        flow_.field("feasibility",
                    preflight.feasibility == contracts::RestoreFeasibility::kEligible
                        ? "eligible"
                        : "provisional");
        flow_.field_bytes("minimum_target", preflight.minimum_target_bytes);
        flow_.field_u64("minimum_search_steps", minimum_search_steps_);
        flow_.field_bytes("relocation_bytes", preflight.relocation_bytes);
        flow_.field_bytes("scratch_upper_bound", preflight.scratch_upper_bound_bytes);
        if (!preflight.message_code.empty()) {
            flow_.field("message_code", preflight.message_code);
        }
    }

    void candidate_geometry(
        const ntfs_resize::NtfsShrinkCandidateSnapshot& snapshot) noexcept override {
        if (geometry_logged_) {
            return;
        }
        geometry_logged_ = true;
        const auto ntfs_volume_bytes = snapshot.source_geometry.volume_size_bytes.value;
        const auto source_sector_bytes = snapshot.source_geometry.bytes_per_sector;
        const auto required_device_bytes =
            ntfs_volume_bytes <= (std::numeric_limits<std::uint64_t>::max)() - source_sector_bytes
                ? ntfs_volume_bytes + source_sector_bytes
                : (std::numeric_limits<std::uint64_t>::max)();
        const auto source_size_difference =
            snapshot.expected_source_logical_size_bytes >= required_device_bytes
                ? snapshot.expected_source_logical_size_bytes - required_device_bytes
                : required_device_bytes - snapshot.expected_source_logical_size_bytes;
        flow_.section("SourceGeometry");
        flow_.field_bytes("source_ntfs_volume", ntfs_volume_bytes);
        flow_.field_bytes("source_required_device", required_device_bytes);
        flow_.field_bytes("source_size_difference", source_size_difference);
        flow_.field_u64("source_sector_bytes", source_sector_bytes);
        flow_.field_u64("source_cluster_bytes", snapshot.source_geometry.bytes_per_cluster);
        flow_.field_u64("source_total_clusters", snapshot.source_geometry.total_clusters);
        flow_.field_bool("source_target_sector_match",
                         source_sector_bytes == snapshot.target_geometry.logical_sector_size);
    }

    void candidate_counts(
        const ntfs_resize::NtfsShrinkCountSnapshot& snapshot) noexcept override {
        counts_ = snapshot;
    }

    void candidate_allocation(
        const ntfs_resize::NtfsShrinkAllocationSnapshot& snapshot) noexcept override {
        allocation_ = snapshot;
    }

    void candidate_stage(const std::uint64_t target_capacity_bytes,
                         const std::string_view stage) noexcept override {
        (void)target_capacity_bytes;
        active_stage_ = stage;
    }

    void mft_record(const ntfs_resize::NtfsShrinkMftRecordSnapshot& snapshot) noexcept override {
        if (snapshot.parsed) {
            return;
        }
        flow_.section("MftRecord");
        flow_.field_bytes("candidate_capacity", snapshot.target_capacity_bytes);
        flow_.field_u64("record_number", snapshot.record_number);
        flow_.field("read_path",
                    snapshot.read_via_mft_data ? "mft_data_runlist" : "boot_mft_lcn");
        flow_.field_bytes("mft_file_offset", snapshot.mft_file_offset_bytes);
        if (snapshot.source_device_offset_known) {
            flow_.field_bytes("source_device_offset", snapshot.source_device_offset_bytes);
        }
        std::ostringstream signature;
        signature << std::hex << std::setw(8) << std::setfill('0') << snapshot.signature_hex;
        flow_.field("signature_hex", signature.str());
        flow_.field("result", "rejected");
        flow_.field("error_code", base::error_code_name(snapshot.error_code));
        flow_.field("message_code", snapshot.message_code);
    }

  private:
    [[nodiscard]] static std::chrono::milliseconds
    elapsed_since(const std::chrono::steady_clock::time_point started) noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
    }

    void begin_timed_stage(const std::string_view stage) noexcept {
        open_stage_ = stage;
        stage_started_ = std::chrono::steady_clock::now();
        flow_.stage_begin(stage);
    }

    void end_timed_stage_ok() noexcept {
        if (open_stage_.empty()) {
            return;
        }
        flow_.stage_ok(open_stage_, elapsed_since(stage_started_));
        open_stage_ = {};
    }

    void end_timed_stage_fail(const std::string_view error_code,
                              const std::string_view message_code) noexcept {
        if (open_stage_.empty()) {
            return;
        }
        flow_.stage_fail(open_stage_, elapsed_since(stage_started_), error_code, message_code);
        open_stage_ = {};
    }

    detail::PlainFlowLog flow_;
    std::string_view recovery_point_id_;
    std::uint32_t source_volume_index_;
    std::string_view target_source_id_;
    std::string_view active_phase_;
    std::string_view active_stage_;
    std::string_view open_stage_;
    std::optional<ntfs_resize::NtfsShrinkCountSnapshot> counts_;
    std::optional<ntfs_resize::NtfsShrinkAllocationSnapshot> allocation_;
    std::uint32_t minimum_search_steps_{0};
    bool geometry_logged_{false};
    std::chrono::steady_clock::time_point stage_started_{};
};

/// Matches Worker personal_archive_restore_shrink path_to_utf8 (generic_u8string).
[[nodiscard]] std::string volume_path_utf8_for_digest(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const auto value : encoded) {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

[[nodiscard]] base::Result<std::string> sha256_hex_lowercase(const std::string_view input) {
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(input.data()), input.size());
    auto digest = adapters::crypto_sodium::sha256(bytes);
    if (!digest) {
        return base::Result<std::string>::failure(digest.error());
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.resize(digest.value().size() * 2);
    for (std::size_t index = 0; index < digest.value().size(); ++index) {
        const auto value = static_cast<unsigned char>(digest.value()[index]);
        hex[index * 2] = kHex[value >> 4];
        hex[index * 2 + 1] = kHex[value & 0x0f];
    }
    return base::Result<std::string>::success(std::move(hex));
}

[[nodiscard]] base::Result<std::filesystem::path>
resolve_volume_guid_path(application::ISourceInventoryQuery& inventory,
                         const std::string_view target_source_id,
                         const base::CancellationToken cancellation) {
    auto resolved = inventory.resolve_source(target_source_id, cancellation);
    if (!resolved) {
        return base::Result<std::filesystem::path>::failure(resolved.error());
    }
    auto path = path_from_utf8(resolved.value().stable_key);
    if (!path) {
        return base::Result<std::filesystem::path>::failure(path.error());
    }
    if (!adapters::windows_disk::WindowsBlockSink::is_canonical_volume_guid_path(path.value())) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kConflict, "restore target is not a canonical Volume GUID path"});
    }
    return path;
}

[[nodiscard]] base::Result<adapters::personal_archive::ArchiveChainOpenRequest>
build_chain_open_request(const std::string& locator,
                         const std::vector<personal_repository::CatalogEntry>& chain_entries,
                         const std::string& archive_password,
                         std::vector<std::filesystem::path>& owned_paths) {
    adapters::personal_archive::ArchiveChainOpenRequest open_request;
    open_request.layers.reserve(chain_entries.size());
    owned_paths.clear();
    owned_paths.reserve(chain_entries.size());
    for (const auto& entry : chain_entries) {
        auto absolute = resolve_archive_absolute_path(locator, entry.archive_main_key);
        if (!absolute) {
            return base::Result<adapters::personal_archive::ArchiveChainOpenRequest>::failure(
                absolute.error());
        }
        auto path = path_from_utf8(absolute.value());
        if (!path) {
            return base::Result<adapters::personal_archive::ArchiveChainOpenRequest>::failure(
                path.error());
        }
        owned_paths.push_back(std::move(path).value());
        adapters::personal_archive::ArchiveOpenRequest layer;
        layer.source = owned_paths.back();
        layer.password = archive_password;
        open_request.layers.push_back(std::move(layer));
    }
    return base::Result<adapters::personal_archive::ArchiveChainOpenRequest>::success(
        std::move(open_request));
}

[[nodiscard]] std::uint64_t
sum_relocation_bytes(const ntfs_resize::ShrinkPlan& plan) noexcept {
    const auto cluster_size = plan.source_ntfs_geometry().bytes_per_cluster;
    std::uint64_t total = 0;
    for (const auto& record : plan.relocation_records()) {
        const auto bytes = record.cluster_count * static_cast<std::uint64_t>(cluster_size);
        total += bytes;
    }
    return total;
}

[[nodiscard]] base::Result<contracts::RestorePreflight>
map_analyze_failure(const base::Error& error) {
    std::string code = error.message;
    if (!code.starts_with("restore.shrink_") && !code.starts_with("ntfs_resize.")) {
        code = "restore.shrink_unsupported_layout";
    }
    return base::Result<contracts::RestorePreflight>::failure(
        base::Error{error.code == base::ErrorCode::kNone ? base::ErrorCode::kConflict : error.code,
                    std::move(code)});
}

struct ShrinkTargetContext final {
    std::uint64_t inventory_capacity{0};
    std::filesystem::path volume_guid_path;
};

[[nodiscard]] base::Result<ShrinkTargetContext>
resolve_shrink_target(application::ISourceInventoryQuery& inventory,
                      const contracts::RestorePreflightRequest& request,
                      const base::CancellationToken cancellation) {
    if (request.volume_size_policy != contracts::VolumeSizePolicy::kAllowNtfsRelocation) {
        return base::Result<ShrinkTargetContext>::failure(
            {base::ErrorCode::kInvalidArgument,
             "analyze_ntfs_shrink requires volume_size_policy allow_ntfs_relocation"});
    }
    if (is_disk_target_id(request.target_source_id) ||
        !is_volume_target_id(request.target_source_id)) {
        return base::Result<ShrinkTargetContext>::failure(
            {base::ErrorCode::kInvalidArgument,
             "analyze_ntfs_shrink requires a volume target_source_id"});
    }
    auto target = find_inventory_item(inventory, request.target_source_id, cancellation);
    if (!target) {
        return base::Result<ShrinkTargetContext>::failure(target.error());
    }
    if (target.value().is_system) {
        return base::Result<ShrinkTargetContext>::failure(
            {base::ErrorCode::kConflict, "system volume restore requires PE (not available online)"});
    }
    if (target.value().is_read_only) {
        return base::Result<ShrinkTargetContext>::failure(
            {base::ErrorCode::kConflict, "restore target volume is read-only"});
    }
    if (target.value().availability != contracts::SourceAvailability::kAvailable) {
        return base::Result<ShrinkTargetContext>::failure(
            {base::ErrorCode::kConflict, "restore target is unavailable"});
    }
    if (target.value().capacity_bytes == 0) {
        return base::Result<ShrinkTargetContext>::failure(
            {base::ErrorCode::kConflict, "restore target capacity is unavailable"});
    }
    auto volume_guid = resolve_volume_guid_path(inventory, request.target_source_id, cancellation);
    if (!volume_guid) {
        return base::Result<ShrinkTargetContext>::failure(volume_guid.error());
    }
    ShrinkTargetContext context;
    context.inventory_capacity = target.value().capacity_bytes;
    context.volume_guid_path = std::move(volume_guid).value();
    return base::Result<ShrinkTargetContext>::success(std::move(context));
}

struct OpenedShrinkSource final {
    std::vector<std::filesystem::path> owned_paths;
    std::unique_ptr<adapters::personal_archive::PersonalArchiveChainReader> chain;
    std::unique_ptr<adapters::personal_archive::PersonalArchiveVolumeRandomReader> volume_random;
    std::vector<personal_repository::CatalogEntry> chain_entries;
    std::uint64_t source_size_bytes{0};
};

struct ShrinkAnalyzeDependencies final {
    application::ISourceInventoryQuery& source_inventory;
    ports::IControlPlaneDatabase& control_plane;
    ports::IRepositoryStorageFactory& storage_factory;
    ports::IClock& clock;
    ports::IRandomSource& random;
};

struct PreparedShrinkAnalysis final {
    ShrinkTargetContext target;
    OpenedShrinkSource source;
    std::string target_digest;
    std::string fingerprint;
    ntfs_resize::NtfsShrinkAnalyzeRequest analyze;
};

struct ExactShrinkAnalysis final {
    std::optional<ntfs_resize::ShrinkPlan> target_plan;
    std::uint64_t minimum_target_bytes{0};
    base::Error target_failure;
};

[[nodiscard]] base::Result<ntfs_resize::ShrinkPlan>
analyze_at_capacity(const ntfs_resize::NtfsShrinkAnalyzeRequest& request,
                    const std::uint64_t capacity, const std::string_view phase,
                    ShrinkAnalyzeFlowLog& flow, const base::CancellationToken cancellation) {
    flow.begin_candidate(phase);
    auto candidate = request;
    candidate.target_capacity_bytes = capacity;
    candidate.target_geometry.capacity_bytes = capacity;
    auto result = ntfs_resize::NtfsShrinkAnalyzer::analyze(candidate, cancellation);
    flow.candidate_result(phase, capacity, result ? nullptr : &result.error());
    return result;
}

[[nodiscard]] base::Result<std::uint64_t>
minimum_supported_capacity(const ntfs_resize::NtfsShrinkAnalyzeRequest& request,
                           const ntfs_resize::ShrinkPlan& upper_plan,
                           ShrinkAnalyzeFlowLog& flow,
                           const base::CancellationToken cancellation) {
    const auto cluster_size = upper_plan.source_ntfs_geometry().bytes_per_cluster;
    const auto sector_size = upper_plan.source_ntfs_geometry().bytes_per_sector;
    if (cluster_size == 0 || sector_size == 0 ||
        upper_plan.new_total_cluster_count() < 16U) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"});
    }
    std::uint64_t low = 16U;
    std::uint64_t high = upper_plan.new_total_cluster_count();
    while (low < high) {
        if (cancellation.stop_requested()) {
            return base::Result<std::uint64_t>::failure(
                {base::ErrorCode::kCancelled, "ntfs.read_failed"});
        }
        const auto middle = low + (high - low) / 2U;
        if (middle > ((std::numeric_limits<std::uint64_t>::max)() - sector_size) /
                         cluster_size) {
            return base::Result<std::uint64_t>::failure(
                {base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"});
        }
        const auto candidate_bytes = middle * cluster_size + sector_size;
        flow.search_bounds(low, high, middle, candidate_bytes);
        auto candidate =
            analyze_at_capacity(request, candidate_bytes, "minimum_search", flow, cancellation);
        if (candidate) {
            high = middle;
        } else if (candidate.error().code == base::ErrorCode::kCancelled) {
            return base::Result<std::uint64_t>::failure(candidate.error());
        } else {
            low = middle + 1U;
        }
    }
    if (low > ((std::numeric_limits<std::uint64_t>::max)() - sector_size) / cluster_size) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"});
    }
    return base::Result<std::uint64_t>::success(low * cluster_size + sector_size);
}

[[nodiscard]] base::Result<ExactShrinkAnalysis>
analyze_exact_shrink(const ntfs_resize::NtfsShrinkAnalyzeRequest& request,
                     ShrinkAnalyzeFlowLog& flow,
                     const base::CancellationToken cancellation) {
    ExactShrinkAnalysis result;
    auto target_plan = analyze_at_capacity(request, request.target_capacity_bytes, "target", flow,
                                           cancellation);
    if (target_plan) {
        auto minimum =
            minimum_supported_capacity(request, target_plan.value(), flow, cancellation);
        if (!minimum) {
            return base::Result<ExactShrinkAnalysis>::failure(minimum.error());
        }
        result.minimum_target_bytes = minimum.value();
        result.target_plan = std::move(target_plan).value();
        return base::Result<ExactShrinkAnalysis>::success(std::move(result));
    }
    if (target_plan.error().code == base::ErrorCode::kCancelled) {
        return base::Result<ExactShrinkAnalysis>::failure(target_plan.error());
    }
    result.target_failure = target_plan.error();
    const auto sector_size = request.target_geometry.logical_sector_size;
    if (sector_size == 0 || request.expected_source_logical_size_bytes <= sector_size) {
        return base::Result<ExactShrinkAnalysis>::failure(result.target_failure);
    }
    const auto upper_capacity =
        ((request.expected_source_logical_size_bytes - sector_size) / sector_size) * sector_size;
    auto upper_plan = analyze_at_capacity(request, upper_capacity, "upper_bound", flow,
                                          cancellation);
    if (!upper_plan) {
        return base::Result<ExactShrinkAnalysis>::failure(result.target_failure);
    }
    auto minimum = minimum_supported_capacity(request, upper_plan.value(), flow, cancellation);
    if (!minimum) {
        return base::Result<ExactShrinkAnalysis>::failure(minimum.error());
    }
    result.minimum_target_bytes = minimum.value();
    return base::Result<ExactShrinkAnalysis>::success(std::move(result));
}

[[nodiscard]] base::Result<OpenedShrinkSource>
open_shrink_source(ports::IControlPlaneDatabase& control_plane,
                   ports::IRepositoryStorageFactory& storage_factory,
                   const contracts::RestorePreflightRequest& request,
                   const base::CancellationToken cancellation) {
    auto chain_entries =
        load_restore_chain_or_fail(control_plane, storage_factory, request.repository_connection_id,
                                   request.recovery_point_id, cancellation);
    if (!chain_entries) {
        return base::Result<OpenedShrinkSource>::failure(chain_entries.error());
    }
    auto repository =
        control_plane.get_repository_connection(request.repository_connection_id, cancellation);
    if (!repository || !repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<OpenedShrinkSource>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    OpenedShrinkSource opened;
    opened.chain_entries = std::move(chain_entries).value();
    auto open_request = build_chain_open_request(repository.value()->locator, opened.chain_entries,
                                                 request.archive_password, opened.owned_paths);
    if (!open_request) {
        return base::Result<OpenedShrinkSource>::failure(open_request.error());
    }
    auto chain = adapters::personal_archive::PersonalArchiveChainReader::open(open_request.value());
    if (!chain) {
        return base::Result<OpenedShrinkSource>::failure(chain.error());
    }
    opened.chain = std::move(chain).value();
    auto volume_random = adapters::personal_archive::PersonalArchiveVolumeRandomReader::open(
        *opened.chain, opened.chain->manifest(), request.source_volume_index);
    if (!volume_random) {
        return base::Result<OpenedShrinkSource>::failure(volume_random.error());
    }
    opened.volume_random = std::move(volume_random).value();
    opened.source_size_bytes = opened.volume_random->size_bytes();
    if (opened.source_size_bytes == 0) {
        return base::Result<OpenedShrinkSource>::failure(
            {base::ErrorCode::kConflict, "source volume size is unavailable"});
    }
    return base::Result<OpenedShrinkSource>::success(std::move(opened));
}

[[nodiscard]] base::Result<PreparedShrinkAnalysis>
prepare_shrink_analysis(const ShrinkAnalyzeDependencies& dependencies,
                        const contracts::RestorePreflightRequest& request,
                        ShrinkAnalyzeFlowLog& flow,
                        const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_restore_preflight_request(request); !valid) {
        flow.failed("validate_request", valid.error());
        return base::Result<PreparedShrinkAnalysis>::failure(valid.error());
    }
    auto target = resolve_shrink_target(dependencies.source_inventory, request, cancellation);
    if (!target) {
        flow.failed("resolve_target", target.error());
        return base::Result<PreparedShrinkAnalysis>::failure(target.error());
    }
    auto source = open_shrink_source(dependencies.control_plane, dependencies.storage_factory,
                                     request, cancellation);
    if (!source) {
        flow.failed("open_source", source.error());
        return base::Result<PreparedShrinkAnalysis>::failure(source.error());
    }
    if (target.value().inventory_capacity >= source.value().source_size_bytes) {
        const base::Error error{base::ErrorCode::kInvalidArgument,
                                "analyze_ntfs_shrink requires a smaller target than source"};
        flow.failed("compare_capacity", error);
        return base::Result<PreparedShrinkAnalysis>::failure(error);
    }
    auto geometry =
        adapters::windows_disk::probe_volume_block_geometry(target.value().volume_guid_path);
    if (!geometry) {
        flow.failed("probe_target_geometry", geometry.error());
        return base::Result<PreparedShrinkAnalysis>::failure(geometry.error());
    }
    if (geometry.value().capacity_bytes != target.value().inventory_capacity) {
        const base::Error error{base::ErrorCode::kConflict, "restore.shrink_plan_changed"};
        flow.failed("compare_target_snapshot", error);
        return base::Result<PreparedShrinkAnalysis>::failure(error);
    }
    flow.inputs_ready(source.value().source_size_bytes, source.value().chain_entries.size(),
                      geometry.value());
    auto target_digest =
        sha256_hex_lowercase(volume_path_utf8_for_digest(target.value().volume_guid_path));
    if (!target_digest) {
        flow.failed("bind_target", target_digest.error());
        return base::Result<PreparedShrinkAnalysis>::failure(target_digest.error());
    }
    VolumeRestoreChain volume_chain;
    volume_chain.source_volume_index = request.source_volume_index;
    volume_chain.volume_size_bytes = source.value().source_size_bytes;
    volume_chain.layers.reserve(source.value().chain_entries.size());
    for (const auto& entry : source.value().chain_entries) {
        volume_chain.layers.push_back({entry.archive_main_key, entry.file_uuid});
    }
    PreparedShrinkAnalysis prepared;
    prepared.target = std::move(target).value();
    prepared.source = std::move(source).value();
    prepared.target_digest = std::move(target_digest).value();
    prepared.fingerprint = make_volume_restore_fingerprint(volume_chain);
    prepared.analyze = {prepared.source.volume_random.get(),
                        prepared.source.source_size_bytes,
                        request.source_volume_index,
                        prepared.fingerprint,
                        prepared.target.inventory_capacity,
                        geometry.value(),
                        prepared.target_digest,
                        &flow};
    return base::Result<PreparedShrinkAnalysis>::success(std::move(prepared));
}

[[nodiscard]] PreparedRestoreChain
make_eligible_shrink_preflight(const OpenedShrinkSource& source,
                                const ShrinkTargetContext& target, const std::string& fingerprint,
                                const ntfs_resize::ShrinkPlan& plan,
                                const std::uint64_t minimum_target_bytes,
                                std::string target_digest) {
    PreparedRestoreChain prepared;
    prepared.repository_uuid = source.chain_entries.back().repository_uuid;
    prepared.chain_fingerprint = fingerprint;
    prepared.logical_size_bytes = source.source_size_bytes;
    prepared.target_capacity_bytes = target.inventory_capacity;
    prepared.chain_depth = static_cast<std::uint32_t>(source.chain_entries.size());
    prepared.volume_size_policy = contracts::VolumeSizePolicy::kAllowNtfsRelocation;
    prepared.feasibility = contracts::RestoreFeasibility::kEligible;
    prepared.minimum_target_bytes = minimum_target_bytes;
    prepared.relocation_bytes = sum_relocation_bytes(plan);
    prepared.scratch_upper_bound_bytes = plan.scratch_upper_bound_bytes();
    prepared.shrink_plan_digest = plan.plan_payload_digest();
    prepared.target_binding_digest = std::move(target_digest);
    prepared.message_code = "restore.shrink_plan_ready";
    return prepared;
}

[[nodiscard]] PreparedRestoreChain
make_insufficient_shrink_preflight(const OpenedShrinkSource& source,
                                   const ShrinkTargetContext& target,
                                   const std::string& fingerprint,
                                   const std::uint64_t minimum_target_bytes,
                                   std::string target_digest) {
    PreparedRestoreChain prepared;
    prepared.repository_uuid = source.chain_entries.back().repository_uuid;
    prepared.chain_fingerprint = fingerprint;
    prepared.logical_size_bytes = source.source_size_bytes;
    prepared.target_capacity_bytes = target.inventory_capacity;
    prepared.chain_depth = static_cast<std::uint32_t>(source.chain_entries.size());
    prepared.volume_size_policy = contracts::VolumeSizePolicy::kAllowNtfsRelocation;
    prepared.feasibility = contracts::RestoreFeasibility::kProvisional;
    prepared.minimum_target_bytes = minimum_target_bytes;
    prepared.target_binding_digest = std::move(target_digest);
    prepared.restriction_codes = {"restore.shrink_below_minimum"};
    prepared.message_code = "restore.shrink_below_minimum";
    return prepared;
}

[[nodiscard]] base::Result<contracts::RestorePreflight>
execute_shrink_analysis(const ShrinkAnalyzeDependencies& dependencies,
                        PreparedShrinkAnalysis& prepared,
                        const contracts::RestorePreflightRequest& request,
                        ShrinkAnalyzeFlowLog& flow,
                        const base::CancellationToken cancellation) {
    auto exact = analyze_exact_shrink(prepared.analyze, flow, cancellation);
    if (!exact) {
        flow.failed("analyze_candidates", exact.error());
        return map_analyze_failure(exact.error());
    }
    PreparedRestoreChain preflight;
    if (exact.value().target_plan) {
        preflight = make_eligible_shrink_preflight(
            prepared.source, prepared.target, prepared.fingerprint, *exact.value().target_plan,
            exact.value().minimum_target_bytes, std::move(prepared.target_digest));
    } else {
        preflight = make_insufficient_shrink_preflight(
            prepared.source, prepared.target, prepared.fingerprint,
            exact.value().minimum_target_bytes, std::move(prepared.target_digest));
    }
    auto persisted = persist_restore_preflight(dependencies.control_plane, dependencies.clock,
                                               dependencies.random, request, std::move(preflight),
                                               cancellation);
    if (!persisted) {
        flow.failed("persist_preflight", persisted.error());
        return persisted;
    }
    flow.completed(persisted.value());
    return persisted;
}

} // namespace

base::Result<contracts::RestorePreflight>
WorkerJobService::analyze_ntfs_shrink(const contracts::RestorePreflightRequest& request,
                                      const base::CancellationToken cancellation) {
    ShrinkAnalyzeFlowLog flow(logger_, request);
    flow.started();
    const ShrinkAnalyzeDependencies dependencies{source_inventory_, control_plane_,
                                                 storage_factory_, clock_, random_};
    auto prepared = prepare_shrink_analysis(dependencies, request, flow, cancellation);
    if (!prepared) {
        return base::Result<contracts::RestorePreflight>::failure(prepared.error());
    }
    return execute_shrink_analysis(dependencies, prepared.value(), request, flow, cancellation);
}

} // namespace aegra::apps::service
