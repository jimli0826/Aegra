#include "personal_archive_restore_shrink.h"

#include "worker_task_log.h"

#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/storage_local/windows_scratch_store.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/ntfs_resize/boot_sector_invalidation.h"
#include "aegra/ntfs_resize/composite_ntfs_block_device.h"
#include "aegra/ntfs_resize/ntfs_critical_relocation.h"
#include "aegra/ntfs_resize/ntfs_ordinary_relocation.h"
#include "aegra/ntfs_resize/ntfs_precommit_auditor.h"
#include "aegra/ntfs_resize/ntfs_shrink_analyzer.h"
#include "aegra/ntfs_resize/ntfs_shrink_finalize.h"
#include "aegra/pipeline/protected_range_block_sink.h"
#include "aegra/pipeline/restore_pipeline.h"
#include "aegra/ports/random_access_block_device.h"
#include "aegra/ports/scratch_store.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::worker::detail {
namespace {

constexpr std::uint64_t kDefaultScratchMemoryBudgetBytes = 64ull * 1024ull * 1024ull;
constexpr std::size_t kMaximumJobPathComponentLength = 128;

[[nodiscard]] base::Error
fail_stage(ScopedStage& stage, const base::Error& error, const std::string_view step) {
    stage.fail(error, step);
    return error;
}

template <typename T>
[[nodiscard]] base::Result<T>
stage_failure(ScopedStage& stage, const base::Error& error, const std::string_view step) {
    return base::Result<T>::failure(fail_stage(stage, error, step));
}

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const auto value : encoded) {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

// Keeps native separators. Volume GUID paths (\\?\Volume{...}\) must not be converted to
// forward slashes: CHKDSK and Win32 volume-name comparisons reject the generic form.
[[nodiscard]] std::string path_to_native_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
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

[[nodiscard]] std::filesystem::path resolve_data_dir_for_staging() {
    const auto read_env = [](const wchar_t* name) -> std::filesystem::path {
        const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) {
            return {};
        }
        std::vector<wchar_t> value(required);
        const DWORD written = ::GetEnvironmentVariableW(name, value.data(), required);
        if (written == 0 || written >= required) {
            return {};
        }
        return std::filesystem::path(value.data());
    };
    if (const auto from_service = read_env(L"AEGRA_DATA_DIR"); !from_service.empty()) {
        return from_service;
    }
    if (const auto local = read_env(L"LOCALAPPDATA"); !local.empty()) {
        return local / L"Aegra";
    }
    if (const auto program_data = read_env(L"ProgramData"); !program_data.empty()) {
        return program_data / L"Aegra";
    }
    return {};
}

struct PreparedShrinkScratchDirectory final {
    std::filesystem::path staging;
    std::filesystem::path job;
    std::filesystem::path shrink;
    std::filesystem::path overlay;
    bool staging_created{false};
    bool job_created{false};
    bool shrink_created{false};
};

class ShrinkScratchDirectoryCleanup final {
  public:
    explicit ShrinkScratchDirectoryCleanup(
        const PreparedShrinkScratchDirectory& prepared) noexcept
        : prepared_(&prepared) {}

    ShrinkScratchDirectoryCleanup(const ShrinkScratchDirectoryCleanup&) = delete;
    ShrinkScratchDirectoryCleanup& operator=(const ShrinkScratchDirectoryCleanup&) = delete;

    ~ShrinkScratchDirectoryCleanup() {
        if (prepared_ == nullptr) {
            return;
        }
        if (prepared_->shrink_created) {
            static_cast<void>(::RemoveDirectoryW(prepared_->shrink.c_str()));
        }
        if (prepared_->job_created) {
            static_cast<void>(::RemoveDirectoryW(prepared_->job.c_str()));
        }
        if (prepared_->staging_created) {
            static_cast<void>(::RemoveDirectoryW(prepared_->staging.c_str()));
        }
    }

    void release() noexcept { prepared_ = nullptr; }

  private:
    const PreparedShrinkScratchDirectory* prepared_;
};

[[nodiscard]] bool is_safe_job_path_component(const std::string_view job_id) noexcept {
    if (job_id.empty() || job_id.size() > kMaximumJobPathComponentLength) {
        return false;
    }
    for (const char value : job_id) {
        const bool is_ascii_letter = (value >= 'a' && value <= 'z') ||
                                     (value >= 'A' && value <= 'Z');
        const bool is_ascii_digit = value >= '0' && value <= '9';
        if (!is_ascii_letter && !is_ascii_digit && value != '-' && value != '_') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] base::Error scratch_directory_error(const base::ErrorCode code,
                                                  const std::string_view operation,
                                                  const DWORD win32_error = ERROR_SUCCESS) {
    std::string message(operation);
    if (win32_error != ERROR_SUCCESS) {
        message.append(": win32=");
        message.append(std::to_string(win32_error));
    }
    return {code, std::move(message)};
}

[[nodiscard]] base::Result<std::filesystem::path>
absolute_local_data_directory(const std::filesystem::path& path) {
    const auto native = path.native();
    if (native.empty() || native.starts_with(L"\\\\") || native.starts_with(L"\\??\\")) {
        return base::Result<std::filesystem::path>::failure(scratch_directory_error(
            base::ErrorCode::kInvalidArgument, "scratch data directory must be local absolute"));
    }
    const DWORD required = ::GetFullPathNameW(native.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return base::Result<std::filesystem::path>::failure(scratch_directory_error(
            base::ErrorCode::kIoFailure, "resolve scratch data directory", ::GetLastError()));
    }
    std::wstring full(static_cast<std::size_t>(required), L'\0');
    const DWORD written = ::GetFullPathNameW(native.c_str(), required, full.data(), nullptr);
    if (written == 0 || written >= required) {
        return base::Result<std::filesystem::path>::failure(scratch_directory_error(
            base::ErrorCode::kIoFailure, "resolve scratch data directory", ::GetLastError()));
    }
    full.resize(static_cast<std::size_t>(written));
    if (full.size() < 3 || full[1] != L':' || (full[2] != L'\\' && full[2] != L'/')) {
        return base::Result<std::filesystem::path>::failure(scratch_directory_error(
            base::ErrorCode::kInvalidArgument, "scratch data directory must be local absolute"));
    }
    return base::Result<std::filesystem::path>::success(
        std::filesystem::path(std::move(full)).lexically_normal());
}

[[nodiscard]] base::Result<bool>
ensure_safe_directory(const std::filesystem::path& path, const bool create_missing) {
    auto attributes = ::GetFileAttributesW(path.c_str());
    const DWORD inspection_error = ::GetLastError();
    bool created = false;
    if (attributes == INVALID_FILE_ATTRIBUTES && create_missing &&
        (inspection_error == ERROR_FILE_NOT_FOUND || inspection_error == ERROR_PATH_NOT_FOUND)) {
        if (::CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
            created = true;
        } else {
            const DWORD create_error = ::GetLastError();
            if (create_error == ERROR_ALREADY_EXISTS) {
                attributes = ::GetFileAttributesW(path.c_str());
            } else {
                return base::Result<bool>::failure(scratch_directory_error(
                    base::ErrorCode::kIoFailure, "create scratch directory", create_error));
            }
        }
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            attributes = ::GetFileAttributesW(path.c_str());
        }
    }
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return base::Result<bool>::failure(scratch_directory_error(
            base::ErrorCode::kIoFailure, "inspect scratch directory", ::GetLastError()));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return base::Result<bool>::failure(scratch_directory_error(
            base::ErrorCode::kConflict, "scratch path is not a safe directory"));
    }
    return base::Result<bool>::success(created);
}

[[nodiscard]] base::Result<void>
require_safe_directory_chain(const std::filesystem::path& path) {
    auto current = path.root_path();
    auto safe = ensure_safe_directory(current, false);
    if (!safe) {
        return base::Result<void>::failure(safe.error());
    }
    for (const auto& component : path.relative_path()) {
        current /= component;
        safe = ensure_safe_directory(current, false);
        if (!safe) {
            return base::Result<void>::failure(safe.error());
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<PreparedShrinkScratchDirectory>
prepare_shrink_scratch_directory(const PersonalArchiveRestoreBackendRequest& request) {
    ScopedStage stage(WorkerTaskLog::active(), "prepare_scratch_directory");
    if (!is_safe_job_path_component(request.job_id)) {
        const base::Error error{base::ErrorCode::kInvalidArgument,
                                "restore.shrink_plan_corrupt"};
        return stage_failure<PreparedShrinkScratchDirectory>(stage, error,
                                                              "validate_job_id");
    }
    auto data_dir = absolute_local_data_directory(resolve_data_dir_for_staging());
    if (!data_dir) {
        return stage_failure<PreparedShrinkScratchDirectory>(stage, data_dir.error(),
                                                              "resolve_data_dir");
    }
    if (auto safe = require_safe_directory_chain(data_dir.value()); !safe) {
        return stage_failure<PreparedShrinkScratchDirectory>(stage, safe.error(),
                                                              "validate_data_dir");
    }

    PreparedShrinkScratchDirectory prepared;
    prepared.staging = data_dir.value() / L"staging";
    prepared.job = prepared.staging / std::filesystem::path(request.job_id);
    prepared.shrink = prepared.job / L"ntfs-shrink";
    prepared.overlay = prepared.shrink / L"overlay";
    ShrinkScratchDirectoryCleanup cleanup(prepared);

    auto staging = ensure_safe_directory(prepared.staging, true);
    if (!staging) {
        return stage_failure<PreparedShrinkScratchDirectory>(stage, staging.error(),
                                                              "prepare_staging_dir");
    }
    prepared.staging_created = staging.value();
    auto job = ensure_safe_directory(prepared.job, true);
    if (!job) {
        return stage_failure<PreparedShrinkScratchDirectory>(stage, job.error(),
                                                              "prepare_job_dir");
    }
    prepared.job_created = job.value();
    auto shrink = ensure_safe_directory(prepared.shrink, true);
    if (!shrink) {
        return stage_failure<PreparedShrinkScratchDirectory>(stage, shrink.error(),
                                                              "prepare_shrink_dir");
    }
    prepared.shrink_created = shrink.value();
    stage.note("path", path_to_utf8(prepared.shrink));
    cleanup.release();
    return base::Result<PreparedShrinkScratchDirectory>::success(std::move(prepared));
}

[[nodiscard]] adapters::personal_archive::ArchiveChainOpenRequest
make_chain_open_request(const PersonalArchiveRestoreBackendRequest& request,
                        std::vector<std::filesystem::path>& protected_sources) {
    adapters::personal_archive::ArchiveChainOpenRequest open_request;
    open_request.maximum_chain_depth = request.maximum_chain_depth;
    open_request.layers.reserve(request.layers.size());
    protected_sources.reserve(request.layers.size());
    for (std::size_t index = 0; index < request.layers.size(); ++index) {
        const auto& layer = request.layers[index];
        adapters::personal_archive::ArchiveOpenRequest layer_request;
        layer_request.source = layer.source;
        layer_request.password = layer.password;
        layer_request.maximum_chunk_payload_size = request.maximum_chunk_size;
        layer_request.maximum_chunk_logical_size = request.maximum_chunk_size;
        layer_request.sequential_payload_prefetch = index == 0;
        open_request.layers.push_back(std::move(layer_request));
        protected_sources.push_back(layer.source);
    }
    return open_request;
}

[[nodiscard]] std::vector<pipeline::ProtectedByteRange>
to_protected_byte_ranges(const std::vector<ntfs_resize::ByteRange>& ranges) {
    std::vector<pipeline::ProtectedByteRange> result;
    result.reserve(ranges.size());
    for (const auto& range : ranges) {
        result.push_back(pipeline::ProtectedByteRange{range.begin, range.end});
    }
    return result;
}

class WindowsShrinkVolumePostcheck final : public ntfs_resize::IShrinkVolumePostcheck {
  public:
    explicit WindowsShrinkVolumePostcheck(std::filesystem::path volume_guid_path) noexcept
        : volume_guid_path_(std::move(volume_guid_path)) {}

    [[nodiscard]] base::Result<ntfs_resize::ShrinkVolumePostcheckSnapshot>
    query(base::CancellationToken cancellation) override {
        static_cast<void>(cancellation);
        auto probed = adapters::windows_disk::query_volume_shrink_postcheck(volume_guid_path_);
        if (!probed) {
            return base::Result<ntfs_resize::ShrinkVolumePostcheckSnapshot>::failure(
                probed.error());
        }
        ntfs_resize::ShrinkVolumePostcheckSnapshot snapshot;
        snapshot.query_succeeded = probed.value().query_succeeded;
        snapshot.volume_dirty = probed.value().volume_dirty;
        snapshot.capacity_bytes = probed.value().capacity_bytes;
        snapshot.bytes_per_sector = probed.value().bytes_per_sector;
        return base::Result<ntfs_resize::ShrinkVolumePostcheckSnapshot>::success(snapshot);
    }

  private:
    std::filesystem::path volume_guid_path_;
};

struct ShrinkReaders final {
    std::unique_ptr<adapters::personal_archive::PersonalArchiveChainReader> chain;
    std::unique_ptr<adapters::personal_archive::PersonalArchiveVolumeRandomReader> random;
    std::unique_ptr<adapters::personal_archive::PersonalArchiveVolumeReader> volume;
    std::vector<std::filesystem::path> protected_sources;
};

struct LockedShrinkResult final {
    pipeline::RestoreSummary prefix;
    ntfs_resize::ShrinkFinalizeResult finalize;
};

template <typename T>
[[nodiscard]] base::Result<T> target_incomplete() {
    return base::Result<T>::failure(
        {base::ErrorCode::kConflict, "restore.shrink_target_incomplete"});
}

[[nodiscard]] base::Result<ShrinkReaders>
open_shrink_readers(const PersonalArchiveRestoreBackendRequest& request) {
    ShrinkReaders readers;
    auto open_request = make_chain_open_request(request, readers.protected_sources);
    {
        ScopedStage stage(WorkerTaskLog::active(), "open_chain_reader");
        stage.note_u64("layer_count", request.layers.size());
        auto opened = adapters::personal_archive::PersonalArchiveChainReader::open(open_request);
        if (!opened) {
            return stage_failure<ShrinkReaders>(stage, opened.error(), "open_archive_chain");
        }
        readers.chain = std::move(opened).value();
    }
    const auto& manifest = readers.chain->manifest();
    {
        ScopedStage stage(WorkerTaskLog::active(), "open_volume_readers");
        stage.note_u64("source_volume_index", request.source_volume_index);
        auto random = adapters::personal_archive::PersonalArchiveVolumeRandomReader::open(
            *readers.chain, manifest, request.source_volume_index);
        if (!random) {
            return stage_failure<ShrinkReaders>(stage, random.error(), "open_volume_random_reader");
        }
        auto volume = adapters::personal_archive::PersonalArchiveVolumeReader::open(
            *readers.chain, manifest, request.source_volume_index);
        if (!volume) {
            return stage_failure<ShrinkReaders>(stage, volume.error(), "open_volume_chunk_reader");
        }
        readers.random = std::move(random).value();
        readers.volume = std::move(volume).value();
        stage.note_bytes("logical_size", readers.random->size_bytes());
    }
    return base::Result<ShrinkReaders>::success(std::move(readers));
}

[[nodiscard]] base::Result<ntfs_resize::ShrinkPlan>
analyze_shrink_plan(const PersonalArchiveRestoreBackendRequest& request,
                    ports::IRandomAccessReader& source, const std::uint64_t source_size,
                    const ports::BlockDeviceGeometry& target_geometry,
                    const base::CancellationToken& cancellation) {
    ScopedStage stage(WorkerTaskLog::active(), "analyze_shrink_plan");
    if (request.source_chain_fingerprint.empty()) {
        const base::Error error{base::ErrorCode::kInvalidArgument, "restore.shrink_plan_corrupt"};
        return stage_failure<ntfs_resize::ShrinkPlan>(stage, error, "require_chain_fingerprint");
    }
    auto target_digest = sha256_hex_lowercase(path_to_utf8(request.target));
    if (!target_digest) {
        return stage_failure<ntfs_resize::ShrinkPlan>(stage, target_digest.error(),
                                                      "hash_target_stable_id");
    }
    ntfs_resize::NtfsShrinkAnalyzeRequest analyze;
    analyze.source_volume = &source;
    analyze.expected_source_logical_size_bytes = source_size;
    analyze.source_volume_index = request.source_volume_index;
    analyze.source_chain_fingerprint = request.source_chain_fingerprint;
    analyze.target_capacity_bytes = target_geometry.capacity_bytes;
    analyze.target_geometry = target_geometry;
    analyze.target_stable_id_digest = std::move(target_digest).value();
    stage.note_bytes("source_logical_size", source_size);
    stage.note_bytes("target_capacity", target_geometry.capacity_bytes);
    stage.note_u64("target_logical_sector_bytes", target_geometry.logical_sector_size);
    stage.note_u64("target_physical_sector_bytes", target_geometry.physical_sector_size);
    auto plan = ntfs_resize::NtfsShrinkAnalyzer::analyze(analyze, cancellation);
    if (!plan) {
        return stage_failure<ntfs_resize::ShrinkPlan>(stage, plan.error(), "ntfs_shrink_analyze");
    }
    if (plan.value().plan_payload_digest() != request.shrink_plan_digest) {
        const base::Error error{base::ErrorCode::kConflict, "restore.shrink_plan_changed"};
        return stage_failure<ntfs_resize::ShrinkPlan>(stage, error, "compare_plan_digest");
    }
    stage.note("plan_payload_digest", plan.value().plan_payload_digest());
    stage.note_u64("new_total_clusters", plan.value().new_total_cluster_count());
    stage.note_u64("relocation_records", plan.value().relocation_records().size());
    stage.note_bytes("scratch_upper_bound", plan.value().scratch_upper_bound_bytes());
    return plan;
}

[[nodiscard]] base::Result<std::unique_ptr<ports::IScratchStore>>
open_shrink_scratch(const PersonalArchiveRestoreBackendRequest& request,
                    const ntfs_resize::ShrinkPlan& plan,
                    const std::filesystem::path& overlay_path,
                    const base::CancellationToken& cancellation) {
    ScopedStage stage(WorkerTaskLog::active(), "open_scratch_store");
    ports::ScratchStoreOpenRequest scratch_request;
    scratch_request.path_utf8 = path_to_utf8(overlay_path);
    scratch_request.logical_size_bytes = plan.source_logical_size_bytes();
    scratch_request.maximum_allocation_bytes = plan.scratch_upper_bound_bytes();
    scratch_request.memory_budget_bytes = request.plan.memory_budget_bytes != 0
                                              ? request.plan.memory_budget_bytes
                                              : kDefaultScratchMemoryBudgetBytes;
    scratch_request.forbidden_volume_guid_utf8 = path_to_native_utf8(request.target);
    stage.note("path", scratch_request.path_utf8);
    stage.note_bytes("logical_size", scratch_request.logical_size_bytes);
    adapters::storage_local::WindowsScratchStoreFactory factory;
    auto opened = factory.open(scratch_request, cancellation);
    if (!opened) {
        return stage_failure<std::unique_ptr<ports::IScratchStore>>(stage, opened.error(),
                                                                    "open_scratch");
    }
    return opened;
}

[[nodiscard]] base::Result<pipeline::RestoreSummary>
run_prefix_restore(const PersonalArchiveRestoreBackendRequest& request,
                   ports::IRecoveryPointReader& volume_reader,
                   ports::IRandomAccessBlockDevice& target, const ntfs_resize::ShrinkPlan& plan,
                   const base::CancellationToken& cancellation) {
    ScopedStage stage(WorkerTaskLog::active(), "prefix_restore_pipeline");
    ports::RandomAccessBlockDeviceSink device_sink(target);
    auto protected_ranges = to_protected_byte_ranges(plan.protected_ranges());
    pipeline::ProtectedRangeBlockSink protected_sink(device_sink, std::move(protected_ranges));
    pipeline::RestorePlan restore_plan = request.plan;
    restore_plan.logical_write_limit_bytes = plan.target_capacity_bytes();
    stage.note_bytes("logical_write_limit", restore_plan.logical_write_limit_bytes);
    pipeline::RestorePipeline restore(volume_reader, protected_sink, request.progress);
    auto summary = restore.run(restore_plan, cancellation);
    if (!summary) {
        return stage_failure<pipeline::RestoreSummary>(stage, summary.error(), "pipeline_run");
    }
    stage.note_bytes("restored_bytes", summary.value().restored_bytes);
    return summary;
}

[[nodiscard]] base::Result<void>
run_relocations(ntfs_resize::CompositeNtfsBlockDevice& composite, const ntfs_resize::ShrinkPlan& plan,
                const base::CancellationToken& cancellation) {
    {
        ScopedStage stage(WorkerTaskLog::active(), "ordinary_relocation");
        ntfs_resize::OrdinaryRelocationRequest ordinary;
        ordinary.plan = &plan;
        ordinary.device = &composite;
        auto moved = ntfs_resize::NtfsOrdinaryRelocationExecutor::execute(ordinary, cancellation);
        if (!moved) {
            stage.fail(moved.error(), "ordinary_relocation");
            return base::Result<void>::failure(moved.error());
        }
        stage.note_bytes("verified_moved_bytes", moved.value().verified_moved_bytes);
        stage.note_u64("relocation_count", moved.value().relocation_count);
    }
    {
        ScopedStage stage(WorkerTaskLog::active(), "critical_relocation");
        ntfs_resize::CriticalRelocationRequest critical;
        critical.plan = &plan;
        critical.device = &composite;
        auto moved = ntfs_resize::NtfsCriticalRelocationExecutor::execute(critical, cancellation);
        if (!moved) {
            stage.fail(moved.error(), "critical_relocation");
            return base::Result<void>::failure(moved.error());
        }
        stage.note_bytes("verified_moved_bytes", moved.value().verified_moved_bytes);
        stage.note_bool("logfile_invalidated", moved.value().logfile_invalidated);
        stage.note_bool("mft_mirror_synced", moved.value().mft_mirror_synced);
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
run_precommit_audit(ntfs_resize::CompositeNtfsBlockDevice& composite,
                    ports::IRandomAccessBlockDevice& target, const ntfs_resize::ShrinkPlan& plan,
                    const base::CancellationToken& cancellation) {
    ScopedStage stage(WorkerTaskLog::active(), "precommit_audit");
    ntfs_resize::PrecommitAuditRequest audit_request;
    audit_request.plan = &plan;
    audit_request.device = &composite;
    audit_request.target = &target;
    auto report = ntfs_resize::NtfsPrecommitAuditor::audit(audit_request, cancellation);
    if (!report) {
        stage.fail(report.error(), "precommit_audit");
        return base::Result<void>::failure(report.error());
    }
    if (!report.value().passed) {
        const auto& codes = report.value().failure_codes;
        const auto& details = report.value().failure_details;
        constexpr std::size_t kMaxLoggedAuditFailures = 16;
        stage.note_u64("failure_count", codes.size());
        for (std::size_t index = 0; index < codes.size() && index < kMaxLoggedAuditFailures;
             ++index) {
            std::string line = codes[index];
            if (index < details.size() && !details[index].empty()) {
                line.append(" (");
                line.append(details[index]);
                line.append(")");
            }
            stage.note("failure[" + std::to_string(index) + "]", line);
        }
        if (codes.size() > kMaxLoggedAuditFailures) {
            stage.note_u64("failures_not_logged", codes.size() - kMaxLoggedAuditFailures);
        }
        const auto code = codes.empty() ? std::string{"restore.shrink_plan_corrupt"}
                                        : codes.front();
        const base::Error error{base::ErrorCode::kConflict, code};
        stage.fail(error, "precommit_failed");
        return base::Result<void>::failure(error);
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<pipeline::RestoreSummary>
map_finalize_outcome(const ntfs_resize::ShrinkFinalizeResult& finalize,
                     const std::uint64_t restored_bytes, const pipeline::RestoreSummary& prefix) {
    if (finalize.outcome == ntfs_resize::ShrinkFinalizeOutcome::kCompleted) {
        pipeline::RestoreSummary summary = prefix;
        summary.restored_bytes = restored_bytes;
        return base::Result<pipeline::RestoreSummary>::success(summary);
    }
    base::ErrorCode code = base::ErrorCode::kIoFailure;
    switch (finalize.outcome) {
    case ntfs_resize::ShrinkFinalizeOutcome::kReadyForPostcheck:
        code = base::ErrorCode::kInternal;
        break;
    case ntfs_resize::ShrinkFinalizeOutcome::kTargetIncomplete:
        code = base::ErrorCode::kConflict;
        break;
    case ntfs_resize::ShrinkFinalizeOutcome::kCommitOutcomeUnknown:
        code = base::ErrorCode::kConflict;
        break;
    case ntfs_resize::ShrinkFinalizeOutcome::kPostcheckFailed:
        code = base::ErrorCode::kConflict;
        break;
    case ntfs_resize::ShrinkFinalizeOutcome::kFailed:
    case ntfs_resize::ShrinkFinalizeOutcome::kCompleted:
        break;
    }
    return base::Result<pipeline::RestoreSummary>::failure({code, finalize.message_code});
}

[[nodiscard]] base::Result<ntfs_resize::ShrinkFinalizeResult>
run_locked_finalize(ports::IRandomAccessBlockDevice& target,
                    ntfs_resize::CompositeNtfsBlockDevice& composite,
                    const ntfs_resize::ShrinkPlan& plan,
                    const base::CancellationToken& cancellation) {
    ScopedStage stage(WorkerTaskLog::active(), "shrink_locked_finalize");
    ntfs_resize::ShrinkFinalizeRequest finalize_request;
    finalize_request.plan = &plan;
    finalize_request.target = &target;
    finalize_request.composite = &composite;
    auto finalize = ntfs_resize::NtfsShrinkFinalizeExecutor::execute_locked_commit(
        finalize_request, cancellation);
    if (!finalize) {
        return stage_failure<ntfs_resize::ShrinkFinalizeResult>(stage, finalize.error(),
                                                                "locked_finalize_execute");
    }
    stage.note_u64("outcome", static_cast<std::uint64_t>(finalize.value().outcome));
    stage.note_u64("reached_phase", static_cast<std::uint64_t>(finalize.value().reached));
    stage.note("message_code", finalize.value().message_code);
    if (!finalize.value().failure_detail.empty()) {
        stage.note("failure_detail", finalize.value().failure_detail);
    }
    return finalize;
}

[[nodiscard]] base::Result<pipeline::RestoreSummary>
run_postcommit(const PersonalArchiveRestoreBackendRequest& request,
               const ntfs_resize::ShrinkPlan& plan, const pipeline::RestoreSummary& prefix,
               ntfs_resize::ShrinkFinalizeResult locked_commit,
               const base::CancellationToken& cancellation) {
    ScopedStage stage(WorkerTaskLog::active(), "shrink_postcommit");
    WindowsShrinkVolumePostcheck postcheck(request.target);
    ntfs_resize::ShrinkPostcommitRequest postcommit_request;
    postcommit_request.plan = &plan;
    postcommit_request.postcheck = &postcheck;
    stage.note_bool("chkdsk_skipped", true);
    auto finalize = ntfs_resize::NtfsShrinkFinalizeExecutor::execute_postcommit(
        postcommit_request, std::move(locked_commit), cancellation);
    if (!finalize) {
        return stage_failure<pipeline::RestoreSummary>(stage, finalize.error(),
                                                       "postcommit_execute");
    }
    stage.note_u64("outcome", static_cast<std::uint64_t>(finalize.value().outcome));
    stage.note_u64("reached_phase", static_cast<std::uint64_t>(finalize.value().reached));
    stage.note("message_code", finalize.value().message_code);
    if (!finalize.value().failure_detail.empty()) {
        stage.note("failure_detail", finalize.value().failure_detail);
    }
    auto mapped = map_finalize_outcome(finalize.value(), plan.target_capacity_bytes(), prefix);
    if (!mapped) {
        stage.fail(mapped.error(), "finalize_outcome");
    }
    return mapped;
}

[[nodiscard]] base::Result<std::unique_ptr<adapters::windows_disk::WindowsRandomAccessBlockDevice>>
open_locked_target(const PersonalArchiveRestoreBackendRequest& request,
                   const ShrinkReaders& readers,
                   const ports::BlockDeviceGeometry& probe_geometry) {
    ScopedStage stage(WorkerTaskLog::active(), "open_target_device");
    stage.note("target", path_display(request.target));
    adapters::windows_disk::WindowsRandomAccessBlockDeviceOpenRequest open_request;
    open_request.path = request.target;
    open_request.kind = adapters::windows_disk::WindowsBlockSinkKind::kVolume;
    open_request.protected_sources = readers.protected_sources;
    open_request.expected_capacity_bytes = probe_geometry.capacity_bytes;
    auto opened = adapters::windows_disk::WindowsRandomAccessBlockDevice::open(open_request);
    if (!opened) {
        return stage_failure<
            std::unique_ptr<adapters::windows_disk::WindowsRandomAccessBlockDevice>>(
            stage, opened.error(), "open_random_access_device");
    }
    stage.note_bytes("capacity", opened.value()->geometry().capacity_bytes);
    return opened;
}

[[nodiscard]] base::Result<LockedShrinkResult>
run_locked_restore(const PersonalArchiveRestoreBackendRequest& request, ShrinkReaders& readers,
                   const ports::BlockDeviceGeometry& probe_geometry,
                   const ntfs_resize::ShrinkPlan& plan,
                   const base::CancellationToken& cancellation) {
    auto target = open_locked_target(request, readers, probe_geometry);
    if (!target) {
        return base::Result<LockedShrinkResult>::failure(target.error());
    }
    auto scratch_directory = prepare_shrink_scratch_directory(request);
    if (!scratch_directory) {
        return base::Result<LockedShrinkResult>::failure(scratch_directory.error());
    }
    ShrinkScratchDirectoryCleanup scratch_directory_cleanup(scratch_directory.value());
    auto scratch =
        open_shrink_scratch(request, plan, scratch_directory.value().overlay, cancellation);
    if (!scratch) {
        return base::Result<LockedShrinkResult>::failure(scratch.error());
    }
    {
        ScopedStage stage(WorkerTaskLog::active(), "invalidate_boot_sectors");
        auto invalidated = ntfs_resize::invalidate_ntfs_boot_sectors(
            *target.value(), target.value()->geometry().logical_sector_size, cancellation);
        if (!invalidated) {
            stage.fail(invalidated.error(), "invalidate_ntfs_boot_sectors");
            return target_incomplete<LockedShrinkResult>();
        }
    }
    auto prefix = run_prefix_restore(request, *readers.volume, *target.value(), plan, cancellation);
    if (!prefix) {
        return target_incomplete<LockedShrinkResult>();
    }
    ntfs_resize::CompositeNtfsBlockDeviceConfig config;
    config.target = target.value().get();
    config.source = readers.random.get();
    config.overlay = scratch.value().get();
    config.source_logical_size_bytes = readers.random->size_bytes();
    config.protected_ranges = plan.protected_ranges();
    auto created = ntfs_resize::CompositeNtfsBlockDevice::create(config);
    if (!created) {
        ScopedStage stage(WorkerTaskLog::active(), "create_composite_device");
        stage.fail(created.error(), "composite_create");
        return target_incomplete<LockedShrinkResult>();
    }
    auto composite = std::move(created).value();
    if (auto moved = run_relocations(composite, plan, cancellation); !moved) {
        return target_incomplete<LockedShrinkResult>();
    }
    if (auto audited = run_precommit_audit(composite, *target.value(), plan, cancellation);
        !audited) {
        return target_incomplete<LockedShrinkResult>();
    }
    auto finalized = run_locked_finalize(*target.value(), composite, plan, cancellation);
    if (!finalized) {
        return target_incomplete<LockedShrinkResult>();
    }
    LockedShrinkResult result{std::move(prefix).value(), std::move(finalized).value()};
    static_cast<void>(scratch.value()->close_and_discard());
    return base::Result<LockedShrinkResult>::success(std::move(result));
}

} // namespace

base::Result<pipeline::RestoreSummary>
run_shrink_volume_restore(const PersonalArchiveRestoreBackendRequest& request,
                          const base::CancellationToken& cancellation) {
    if (request.volume_size_policy != contracts::VolumeSizePolicy::kAllowNtfsRelocation ||
        request.shrink_plan_digest.empty()) {
        return base::Result<pipeline::RestoreSummary>::failure(
            {base::ErrorCode::kInvalidArgument, "restore.shrink_plan_corrupt"});
    }
    if (request.layers.empty()) {
        return base::Result<pipeline::RestoreSummary>::failure(
            {base::ErrorCode::kInvalidArgument, "volume restore requires at least one archive"});
    }
    if (!adapters::windows_disk::WindowsBlockSink::is_canonical_volume_guid_path(request.target)) {
        return base::Result<pipeline::RestoreSummary>::failure(
            {base::ErrorCode::kInvalidArgument, "target is not a canonical Volume GUID path"});
    }

    auto readers = open_shrink_readers(request);
    if (!readers) {
        return base::Result<pipeline::RestoreSummary>::failure(readers.error());
    }

    ports::BlockDeviceGeometry probe_geometry{};
    {
        ScopedStage stage(WorkerTaskLog::active(), "probe_target_geometry");
        stage.note("target", path_display(request.target));
        auto probed = adapters::windows_disk::probe_volume_block_geometry(request.target);
        if (!probed) {
            return stage_failure<pipeline::RestoreSummary>(stage, probed.error(),
                                                           "probe_volume_block_geometry");
        }
        probe_geometry = probed.value();
        stage.note_bytes("capacity", probe_geometry.capacity_bytes);
        stage.note_u64("logical_sector_size", probe_geometry.logical_sector_size);
    }

    auto plan = analyze_shrink_plan(request, *readers.value().random,
                                    readers.value().random->size_bytes(), probe_geometry,
                                    cancellation);
    if (!plan) {
        return base::Result<pipeline::RestoreSummary>::failure(plan.error());
    }
    auto locked = run_locked_restore(request, readers.value(), probe_geometry, plan.value(),
                                     cancellation);
    if (!locked) {
        return base::Result<pipeline::RestoreSummary>::failure(locked.error());
    }
    auto locked_result = std::move(locked).value();
    if (locked_result.finalize.outcome != ntfs_resize::ShrinkFinalizeOutcome::kReadyForPostcheck) {
        return map_finalize_outcome(locked_result.finalize, plan.value().target_capacity_bytes(),
                                    locked_result.prefix);
    }
    return run_postcommit(request, plan.value(), locked_result.prefix,
                          std::move(locked_result.finalize), cancellation);
}

} // namespace aegra::apps::worker::detail
