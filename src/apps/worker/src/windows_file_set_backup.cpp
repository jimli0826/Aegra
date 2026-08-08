#include "windows_file_set_backup.h"

#include "worker_task_log.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/adapters/windows_filesystem/windows_filesystem.h"
#include "aegra/adapters/windows_vss/windows_vss.h"
#include "aegra/format/manifest.h"
#include "aegra/pipeline/file_set_backup_pipeline.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace aegra::apps::worker {
namespace detail {
namespace {

namespace personal_archive = adapters::personal_archive;
namespace windows_disk = adapters::windows_disk;
namespace windows_filesystem = adapters::windows_filesystem;
namespace windows_vss = adapters::windows_vss;

[[nodiscard]] std::wstring lower_native(const std::filesystem::path& path) {
    auto value = path.native();
    std::ranges::transform(value, value.begin(),
                           [](const wchar_t character) { return std::towlower(character); });
    return value;
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

[[nodiscard]] std::vector<std::uint16_t> path_to_utf16(const std::filesystem::path& path) {
    const auto& native = path.native();
    std::vector<std::uint16_t> result;
    result.reserve(native.size());
    for (const wchar_t character : native) {
        result.push_back(static_cast<std::uint16_t>(character));
    }
    return result;
}

[[nodiscard]] base::Result<void> validate_request(const WindowsFileSetBackupRequest& request) {
    if (request.job_id.empty() || request.trace_id.empty() || request.selections.empty() ||
        request.destination.empty() || request.created_utc.empty() ||
        request.index_spool_directory.empty()) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows file_set backup request is incomplete",
        });
    }
    if (request.encryption_enabled == request.password.empty()) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows file_set backup encryption and password are inconsistent",
        });
    }
    if (request.block_size_bytes == 0 || request.chunk_size_bytes < request.block_size_bytes ||
        request.memory_budget_bytes < request.chunk_size_bytes) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows file_set backup geometry is invalid",
        });
    }
    return contracts::validate_file_source_refs(request.selections);
}

[[nodiscard]] base::Result<windows_disk::WindowsVolumeInfo>
resolve_volume_identity(const std::string& volume_identity) {
    std::filesystem::path guid_path;
    {
        std::u8string encoded;
        encoded.reserve(volume_identity.size());
        for (const char item : volume_identity) {
            encoded.push_back(static_cast<char8_t>(item));
        }
        guid_path = std::filesystem::path(encoded);
    }
    if (!windows_disk::WindowsBlockSource::is_canonical_volume_guid_path(guid_path)) {
        return base::Result<windows_disk::WindowsVolumeInfo>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "file_source.volume_identity_mismatch",
        });
    }
    auto inventory = windows_disk::WindowsVolumeEnumerator::enumerate();
    if (!inventory) {
        return base::Result<windows_disk::WindowsVolumeInfo>::failure(inventory.error());
    }
    const auto expected = lower_native(guid_path);
    const auto selected = std::ranges::find_if(inventory.value(), [&expected](const auto& volume) {
        return lower_native(volume.volume_guid_path) == expected;
    });
    if (selected == inventory.value().end()) {
        return base::Result<windows_disk::WindowsVolumeInfo>::failure(
            base::Error{base::ErrorCode::kNotFound, "requested Windows volume was not found"});
    }
    if (!selected->volume_size_available || selected->total_size_bytes == 0) {
        return base::Result<windows_disk::WindowsVolumeInfo>::failure(base::Error{
            base::ErrorCode::kIoFailure,
            "requested Windows volume has no reliable logical size",
        });
    }
    return base::Result<windows_disk::WindowsVolumeInfo>::success(*selected);
}

struct VolumePlan final {
    std::string volume_identity;
    windows_disk::WindowsVolumeInfo volume;
};

[[nodiscard]] base::Result<std::vector<VolumePlan>>
collect_unique_volumes(const std::vector<contracts::FileSourceRef>& selections) {
    std::map<std::string, VolumePlan> ordered;
    for (const auto& selection : selections) {
        if (ordered.contains(selection.volume_identity)) {
            continue;
        }
        auto volume = resolve_volume_identity(selection.volume_identity);
        if (!volume) {
            return base::Result<std::vector<VolumePlan>>::failure(volume.error());
        }
        // File_set requires VSS; no raw fallback (ADR-0016).
        if (!windows_disk::supports_vss_snapshot(volume.value())) {
            return base::Result<std::vector<VolumePlan>>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "file_source.unsupported_filesystem",
            });
        }
        auto supported = windows_vss::is_volume_snapshot_supported(volume.value().volume_guid_path);
        if (!supported) {
            return base::Result<std::vector<VolumePlan>>::failure(supported.error());
        }
        if (!supported.value()) {
            return base::Result<std::vector<VolumePlan>>::failure(base::Error{
                base::ErrorCode::kIoFailure,
                "file_set backup requires VSS support on every selected volume",
            });
        }
        VolumePlan plan;
        plan.volume_identity = selection.volume_identity;
        plan.volume = std::move(volume).value();
        ordered.emplace(plan.volume_identity, std::move(plan));
    }
    std::vector<VolumePlan> result;
    result.reserve(ordered.size());
    for (auto& [_, plan] : ordered) {
        result.push_back(std::move(plan));
    }
    return base::Result<std::vector<VolumePlan>>::success(std::move(result));
}

class SnapshotLease final {
  public:
    explicit SnapshotLease(std::unique_ptr<windows_vss::WindowsVssSnapshotSession> session)
        : session_(std::move(session)) {}

    ~SnapshotLease() {
        if (session_) {
            (void)session_->close();
        }
    }

    SnapshotLease(const SnapshotLease&) = delete;
    SnapshotLease& operator=(const SnapshotLease&) = delete;
    SnapshotLease(SnapshotLease&&) = delete;
    SnapshotLease& operator=(SnapshotLease&&) = delete;

    [[nodiscard]] base::Result<void> close() {
        if (!session_) {
            return base::Result<void>::success();
        }
        auto closed = session_->close();
        session_.reset();
        return closed;
    }

    [[nodiscard]] std::span<const windows_vss::WindowsVssSnapshot> snapshots() const noexcept {
        return session_ ? session_->snapshots()
                        : std::span<const windows_vss::WindowsVssSnapshot>{};
    }

  private:
    std::unique_ptr<windows_vss::WindowsVssSnapshotSession> session_;
};

class StagingDirectory final {
  public:
    explicit StagingDirectory(std::filesystem::path root) : root_(std::move(root)) {}
    ~StagingDirectory() { cleanup(); }
    StagingDirectory(const StagingDirectory&) = delete;
    StagingDirectory& operator=(const StagingDirectory&) = delete;
    StagingDirectory(StagingDirectory&&) = delete;
    StagingDirectory& operator=(StagingDirectory&&) = delete;

    void release() noexcept { root_.clear(); }

    void cleanup() noexcept {
        if (root_.empty()) {
            return;
        }
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
        root_.clear();
    }

  private:
    std::filesystem::path root_;
};

[[nodiscard]] base::Result<std::unique_ptr<SnapshotLease>>
create_vss_lease(const std::vector<VolumePlan>& volumes,
                 const base::CancellationToken& cancellation) {
    std::vector<windows_vss::WindowsVssSnapshotRequest> requests;
    requests.reserve(volumes.size());
    for (const auto& plan : volumes) {
        requests.push_back({plan.volume.volume_guid_path, plan.volume.total_size_bytes});
    }
    auto session = windows_vss::WindowsVssSnapshotSession::create(requests, cancellation);
    if (!session) {
        return base::Result<std::unique_ptr<SnapshotLease>>::failure(session.error());
    }
    if (session.value()->snapshots().size() != volumes.size()) {
        return base::Result<std::unique_ptr<SnapshotLease>>::failure(base::Error{
            base::ErrorCode::kInternal,
            "VSS returned an invalid file_set snapshot count",
        });
    }
    return base::Result<std::unique_ptr<SnapshotLease>>::success(
        std::make_unique<SnapshotLease>(std::move(session).value()));
}

[[nodiscard]] base::Result<std::unique_ptr<windows_filesystem::WindowsFileSnapshotView>>
open_snapshot_view(const std::vector<VolumePlan>& volumes, const SnapshotLease& lease) {
    const auto snapshots = lease.snapshots();
    windows_filesystem::WindowsFileSnapshotOpenRequest open_request;
    open_request.volumes.reserve(volumes.size());
    for (std::size_t index = 0; index < volumes.size(); ++index) {
        windows_filesystem::SnapshotVolumeBinding binding;
        binding.volume_identity = volumes[index].volume_identity;
        binding.snapshot_root_utf16 = path_to_utf16(snapshots[index].snapshot_device_path);
        open_request.volumes.push_back(std::move(binding));
    }
    return windows_filesystem::WindowsFileSnapshotView::open(open_request);
}

[[nodiscard]] format::Manifest make_file_manifest(const WindowsFileSetBackupRequest& request) {
    format::Manifest manifest;
    manifest.content_kind = format::kManifestContentKindFileSet;
    manifest.system.hostname = request.hostname;
    manifest.system.collection_time_utc = request.created_utc;
    manifest.backup_job.backup_type = format::BackupType::kFull;
    manifest.backup_job.created_utc = request.created_utc;
    manifest.backup_job.application_version = request.application_version;
    return manifest;
}

[[nodiscard]] base::Result<std::unique_ptr<ports::IFileBackupSession>>
create_file_archive(const WindowsFileSetBackupRequest& request, const format::Manifest& manifest) {
    personal_archive::FileArchiveCreateRequest archive_request{
        request.destination,
        request.index_spool_directory,
        manifest,
        request.password,
    };
    archive_request.encryption_enabled = request.encryption_enabled;
    archive_request.file_uuid = request.file_uuid;
    archive_request.backup_set_uuid = request.backup_set_uuid;
    archive_request.block_size = request.block_size_bytes;
    archive_request.chunk_size = request.chunk_size_bytes;
    archive_request.split_size_bytes = request.split_size_bytes;
    archive_request.kdf_parameters = {request.kdf_opslimit, request.kdf_memlimit_bytes};
    auto session = personal_archive::PersonalFileArchiveSession::create(archive_request);
    if (!session) {
        return base::Result<std::unique_ptr<ports::IFileBackupSession>>::failure(session.error());
    }
    std::unique_ptr<ports::IFileBackupSession> result = std::move(session).value();
    return base::Result<std::unique_ptr<ports::IFileBackupSession>>::success(std::move(result));
}

[[nodiscard]] base::Result<pipeline::FileSetBackupSummary>
run_pipeline(const WindowsFileSetBackupRequest& request, ports::IFileSnapshotView& snapshot,
             ports::IFileBackupSession& session, ports::IProgressSink* progress,
             const base::CancellationToken& cancellation) {
    pipeline::FileSetBackupPlan plan;
    plan.job_id = request.job_id;
    plan.trace_id = request.trace_id;
    plan.selections = request.selections;
    plan.block_size_bytes = request.block_size_bytes;
    plan.chunk_size_bytes = request.chunk_size_bytes;
    plan.memory_budget_bytes = request.memory_budget_bytes;
    plan.enumerate_batch_size = 256;
    pipeline::FileSetBackupPipeline pipeline(snapshot, session, progress);
    return pipeline.run(plan, cancellation);
}

} // namespace

base::Result<WindowsFileSetBackupResult>
backup_windows_file_set(const WindowsFileSetBackupRequest& request,
                        const base::CancellationToken& cancellation,
                        ports::IProgressSink* progress) {
    try {
        auto validation = validate_request(request);
        if (!validation) {
            return base::Result<WindowsFileSetBackupResult>::failure(validation.error());
        }

        StagingDirectory staging(request.index_spool_directory.parent_path());
        std::vector<VolumePlan> volumes;
        {
            ScopedStage stage(WorkerTaskLog::active(), "prepare_sources");
            auto planned = collect_unique_volumes(request.selections);
            if (!planned) {
                stage.fail(planned.error(), "resolve_volumes",
                           "Confirm selected volumes still exist and support VSS");
                return base::Result<WindowsFileSetBackupResult>::failure(planned.error());
            }
            volumes = std::move(planned).value();
            stage.note_u64("volume_count", volumes.size());
            stage.note_u64("selection_count", request.selections.size());
        }

        std::unique_ptr<SnapshotLease> lease;
        {
            ScopedStage stage(WorkerTaskLog::active(), "create_vss_set");
            auto created = create_vss_lease(volumes, cancellation);
            if (!created) {
                stage.fail(created.error(), "vss_create",
                           "Check VSS service health and that volumes support snapshots");
                return base::Result<WindowsFileSetBackupResult>::failure(created.error());
            }
            lease = std::move(created).value();
            stage.note_u64("snapshot_count", lease->snapshots().size());
        }

        std::unique_ptr<windows_filesystem::WindowsFileSnapshotView> view;
        {
            ScopedStage stage(WorkerTaskLog::active(), "open_snapshot_view");
            auto opened = open_snapshot_view(volumes, *lease);
            if (!opened) {
                stage.fail(opened.error(), "open_view", {});
                return base::Result<WindowsFileSetBackupResult>::failure(opened.error());
            }
            view = std::move(opened).value();
        }

        const auto manifest = make_file_manifest(request);
        std::unique_ptr<ports::IFileBackupSession> session;
        {
            ScopedStage stage(WorkerTaskLog::active(), "create_archive");
            stage.note("destination", path_to_utf8(request.destination));
            stage.note_bool("encryption_enabled", request.encryption_enabled);
            auto opened = create_file_archive(request, manifest);
            if (!opened) {
                stage.fail(opened.error(), "create_session", {});
                return base::Result<WindowsFileSetBackupResult>::failure(opened.error());
            }
            session = std::move(opened).value();
        }

        base::Result<pipeline::FileSetBackupSummary> backup =
            run_pipeline(request, *view, *session, progress, cancellation);
        // Release FS handles before VSS delete (destroy order: pipeline done -> view -> session
        // already committed/aborted inside pipeline -> VSS close).
        view.reset();
        session.reset();
        std::optional<base::Error> cleanup_error;
        {
            auto closed = lease->close();
            if (!closed) {
                cleanup_error = closed.error();
            }
        }
        lease.reset();
        if (!backup) {
            staging.cleanup();
            return base::Result<WindowsFileSetBackupResult>::failure(backup.error());
        }
        staging.cleanup();
        if (cleanup_error) {
            if (auto* log = WorkerTaskLog::active(); log != nullptr) {
                log->warn("Snapshot cleanup reported a non-fatal error after commit");
                log->field("cleanup_error", cleanup_error->message);
            }
        }
        return base::Result<WindowsFileSetBackupResult>::success(
            WindowsFileSetBackupResult{std::move(backup).value(), std::move(cleanup_error)});
    } catch (...) {
        return base::Result<WindowsFileSetBackupResult>::failure(base::Error{
            base::ErrorCode::kInternal,
            "Windows file_set backup failed unexpectedly",
        });
    }
}

} // namespace detail
} // namespace aegra::apps::worker
