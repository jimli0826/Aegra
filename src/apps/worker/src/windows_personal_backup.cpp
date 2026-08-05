#include "aegra/apps/worker/windows_personal_backup.h"

#include "windows_personal_backup_runtime.h"
#include "worker_task_log.h"

#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/contracts/service_control.h"
#include "aegra/format/manifest.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace aegra::apps::worker {
namespace detail {
namespace {

// Service wire integers are signed 64-bit (same bound as contracts/service validation).
constexpr std::uint64_t kMaximumWireInteger =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

[[nodiscard]] base::Result<std::uint64_t> checked_add_wire(const std::uint64_t left,
                                                           const std::uint64_t right) {
    if (left > kMaximumWireInteger || right > kMaximumWireInteger) {
        return base::Result<std::uint64_t>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "volume size exceeds the signed 64-bit wire range",
        });
    }
    if (right > kMaximumWireInteger - left) {
        return base::Result<std::uint64_t>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "multi-volume logical size accumulation overflow",
        });
    }
    return base::Result<std::uint64_t>::success(left + right);
}

bool is_zero_uuid(const std::array<std::byte, 16>& value) noexcept {
    return std::ranges::all_of(value, [](const std::byte item) { return item == std::byte{0}; });
}

base::Result<void> validate_request(const WindowsPersonalBackupRequest& request) {
    if (request.job_id.empty() || request.trace_id.empty() || request.volume_guid_paths.empty() ||
        request.volume_guid_paths.size() > contracts::kMaximumBackupSources ||
        request.destination.empty() || request.created_utc.empty()) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows personal volume backup request is incomplete",
        });
    }
    if (request.encryption_enabled == request.password.empty()) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows personal backup encryption and password are inconsistent",
        });
    }
    std::set<std::filesystem::path> unique_sources;
    for (const auto& source : request.volume_guid_paths) {
        if (source.empty() || !unique_sources.insert(source).second) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "Windows personal backup sources are invalid",
            });
        }
    }
    if (request.block_size_bytes == 0 || request.chunk_size_bytes < request.block_size_bytes ||
        request.memory_budget_bytes < request.chunk_size_bytes) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows personal volume backup geometry is invalid",
        });
    }
    const bool full = request.backup_type == WindowsPersonalBackupType::kFull;
    const bool incremental = request.backup_type == WindowsPersonalBackupType::kIncremental;
    const bool parent_empty = request.parent_source.empty() && request.parent_password.empty();
    const bool parent_complete =
        !request.parent_source.empty() &&
        (request.encryption_enabled ? !request.parent_password.empty() : true);
    if ((!full && !incremental) || (full && !parent_empty) ||
        (incremental && !parent_complete)) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows personal volume backup relationship is invalid",
        });
    }
    if (is_zero_uuid(request.file_uuid) ||
        (full && (is_zero_uuid(request.backup_set_uuid) ||
                  request.file_uuid == request.backup_set_uuid)) ||
        (incremental && !is_zero_uuid(request.backup_set_uuid))) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows personal volume backup identifiers are invalid",
        });
    }
    return base::Result<void>::success();
}

std::string utf8_path(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const auto value : encoded) {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

[[nodiscard]] format::PartitionStyle partition_style_from_name(const std::string_view name) {
    if (name == "MBR") {
        return format::PartitionStyle::kMbr;
    }
    if (name == "GPT") {
        return format::PartitionStyle::kGpt;
    }
    return format::PartitionStyle::kRaw;
}

[[nodiscard]] std::optional<std::uint32_t>
match_partition_number(const format::Disk& disk, const std::uint64_t physical_offset,
                       const std::uint64_t length) {
    for (const auto& partition : disk.partitions) {
        if (partition.size == 0) {
            continue;
        }
        const auto partition_end = partition.offset + partition.size;
        const auto extent_end = physical_offset + length;
        // Extent belongs to partition when its start falls inside the partition range.
        if (physical_offset >= partition.offset && physical_offset < partition_end) {
            return partition.partition_number;
        }
        // Also accept near-full overlap (offset rounding).
        if (physical_offset <= partition.offset && extent_end >= partition_end) {
            return partition.partition_number;
        }
    }
    return std::nullopt;
}

[[nodiscard]] base::Result<format::Disk>
disk_from_layout(const adapters::windows_disk::WindowsPhysicalDiskLayout& layout) {
    format::Disk disk;
    disk.disk_number = layout.disk_number;
    disk.disk_size = layout.disk_size_bytes;
    disk.bytes_per_sector = layout.bytes_per_sector;
    disk.total_sectors = layout.total_sectors;
    disk.partition_style = partition_style_from_name(layout.partition_style);
    disk.model = layout.model;
    disk.serial = layout.serial;
    disk.media_type = layout.media_type;
    disk.partitions.reserve(layout.partitions.size());
    for (const auto& source : layout.partitions) {
        format::Partition partition;
        partition.partition_number = source.partition_number;
        partition.offset = source.offset_bytes;
        partition.size = source.size_bytes;
        partition.style = partition_style_from_name(source.partition_style);
        partition.is_active = source.is_active;
        partition.mbr_type = source.mbr_type;
        partition.gpt_type_guid = source.gpt_type_guid;
        partition.gpt_name = source.gpt_name;
        partition.volume_label = source.volume_label;
        partition.filesystem = source.filesystem;
        partition.volume_guid = source.volume_guid;
        disk.partitions.push_back(std::move(partition));
    }
    return base::Result<format::Disk>::success(std::move(disk));
}

[[nodiscard]] base::Result<format::Manifest>
make_manifest(const WindowsPersonalBackupRequest& request,
              const std::vector<PreparedVolumeMetadata>& sources) {
    format::Manifest manifest;
    manifest.system.hostname = request.hostname;
    manifest.system.collection_time_utc = request.created_utc;
    manifest.backup_job.backup_type =
        request.backup_type == WindowsPersonalBackupType::kFull
            ? format::BackupType::kFull
            : format::BackupType::kIncremental;
    manifest.backup_job.created_utc = request.created_utc;
    manifest.backup_job.application_version = request.application_version;

    std::set<std::uint32_t> disk_numbers;
    for (const auto& metadata : sources) {
        if (metadata.disk_extents.empty()) {
            return base::Result<format::Manifest>::failure(base::Error{
                base::ErrorCode::kIoFailure,
                "selected volume has no physical disk extents for layout metadata",
            });
        }
        for (const auto& extent : metadata.disk_extents) {
            disk_numbers.insert(extent.disk_number);
        }
    }
    std::map<std::uint32_t, format::Disk> disks_by_number;
    for (const auto disk_number : disk_numbers) {
        auto layout = adapters::windows_disk::inspect_physical_disk_layout(disk_number);
        if (!layout) {
            return base::Result<format::Manifest>::failure(layout.error());
        }
        auto disk = disk_from_layout(layout.value());
        if (!disk) {
            return base::Result<format::Manifest>::failure(disk.error());
        }
        disks_by_number.emplace(disk_number, std::move(disk).value());
    }

    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& metadata = sources[index];
        format::Volume volume;
        volume.volume_index = static_cast<std::uint32_t>(index);
        volume.volume_id = utf8_path(metadata.volume_guid_path);
        volume.volume_guid = volume.volume_id;
        volume.filesystem = metadata.filesystem;
        volume.label = metadata.label;
        volume.total_size = metadata.logical_size_bytes;
        volume.cluster_size = metadata.cluster_size_bytes;
        volume.vss_required = metadata.vss_used;
        volume.vss_used = metadata.vss_used;
        volume.consistency_level = metadata.vss_used ? format::ConsistencyLevel::kApplication
                                                     : format::ConsistencyLevel::kCrash;
        for (const auto& mount_point : metadata.mount_points) {
            volume.mount_points.push_back(utf8_path(mount_point));
        }
        std::uint64_t volume_offset = 0;
        for (const auto& disk_extent : metadata.disk_extents) {
            auto disk_it = disks_by_number.find(disk_extent.disk_number);
            if (disk_it == disks_by_number.end()) {
                return base::Result<format::Manifest>::failure(base::Error{
                    base::ErrorCode::kInternal,
                    "volume extent references a disk that was not collected",
                });
            }
            auto partition_number =
                match_partition_number(disk_it->second, disk_extent.disk_offset_bytes,
                                       disk_extent.length_bytes);
            if (!partition_number) {
                return base::Result<format::Manifest>::failure(base::Error{
                    base::ErrorCode::kIoFailure,
                    "volume extent could not be matched to a disk partition",
                });
            }
            for (auto& partition : disk_it->second.partitions) {
                if (partition.partition_number != *partition_number) {
                    continue;
                }
                if (partition.volume_label.empty()) {
                    partition.volume_label = metadata.label;
                }
                if (partition.filesystem.empty()) {
                    partition.filesystem = metadata.filesystem;
                }
                if (partition.volume_guid.empty()) {
                    partition.volume_guid = volume.volume_guid;
                }
                break;
            }
            format::VolumeExtent extent;
            extent.disk_number = disk_extent.disk_number;
            extent.partition_number = *partition_number;
            extent.physical_offset = disk_extent.disk_offset_bytes;
            extent.volume_offset = volume_offset;
            extent.length = disk_extent.length_bytes;
            extent.extent_role = "basic";
            volume.extents.push_back(std::move(extent));
            volume_offset += disk_extent.length_bytes;
        }
        manifest.volumes.push_back(std::move(volume));
    }

    for (auto& [_, disk] : disks_by_number) {
        manifest.disks.push_back(std::move(disk));
    }
    std::sort(manifest.disks.begin(), manifest.disks.end(),
              [](const format::Disk& left, const format::Disk& right) {
                  return left.disk_number < right.disk_number;
              });
    if (manifest.disks.empty() || manifest.volumes.empty()) {
        return base::Result<format::Manifest>::failure(base::Error{
            base::ErrorCode::kInternal,
            "backup layout metadata is incomplete",
        });
    }
    auto valid = format::validate_manifest(manifest);
    if (!valid) {
        return base::Result<format::Manifest>::failure(valid.error());
    }
    return base::Result<format::Manifest>::success(std::move(manifest));
}

std::optional<base::Error> release_snapshot(PreparedVolumeSources& prepared) {
    prepared.sources.clear();
    if (!prepared.snapshot) {
        return std::nullopt;
    }
    auto closed = prepared.snapshot->close();
    if (!closed) {
        return closed.error();
    }
    return std::nullopt;
}

} // namespace

void log_runtime_info(const std::string& message) {
    if (auto* log = WorkerTaskLog::active()) {
        log->info(message);
    }
}

base::Result<pipeline::BackupSummary>
run_volume_pipelines(const WindowsPersonalBackupRequest& request, PreparedVolumeSources& prepared,
                     ports::IBackupSession& session, ports::IProgressSink* progress,
                     const base::CancellationToken& cancellation) {
    pipeline::BackupSummary total;
    std::uint64_t job_logical_bytes = 0;
    for (const auto& metadata : prepared.metadata) {
        auto summed = checked_add_wire(job_logical_bytes, metadata.logical_size_bytes);
        if (!summed) {
            return base::Result<pipeline::BackupSummary>::failure(summed.error());
        }
        job_logical_bytes = summed.value();
    }
    for (std::size_t index = 0; index < prepared.sources.size(); ++index) {
        const auto& metadata = prepared.metadata[index];
        std::ostringstream line;
        line << "Backing up volume " << utf8_path(metadata.volume_guid_path) << " size "
             << metadata.logical_size_bytes << " bytes mode "
             << (metadata.vss_used ? "vss" : "raw");
        log_runtime_info(line.str());
        pipeline::BackupPlan plan{request.job_id, request.trace_id, request.chunk_size_bytes,
                                  request.memory_budget_bytes};
        plan.source_index = static_cast<std::uint32_t>(index);
        plan.commit_mode = index + 1 == prepared.sources.size()
                               ? pipeline::BackupCommitMode::kCommit
                               : pipeline::BackupCommitMode::kDefer;
        plan.progress_total_logical_bytes = job_logical_bytes;
        plan.progress_base_processed_bytes = total.logical_bytes;
        plan.progress_base_stored_bytes = total.stored_bytes;
        // base + this volume must stay within the precomputed job total (and wire range).
        auto window_end =
            checked_add_wire(total.logical_bytes, metadata.logical_size_bytes);
        if (!window_end) {
            return base::Result<pipeline::BackupSummary>::failure(window_end.error());
        }
        if (window_end.value() > job_logical_bytes) {
            return base::Result<pipeline::BackupSummary>::failure(base::Error{
                base::ErrorCode::kInternal,
                "volume progress window exceeds precomputed job logical total",
            });
        }
        pipeline::BackupPipeline pipeline(*prepared.sources[index], session, progress);
        auto backup = pipeline.run(plan, cancellation);
        if (!backup) {
            return base::Result<pipeline::BackupSummary>::failure(backup.error());
        }
        auto next_logical = checked_add_wire(total.logical_bytes, backup.value().logical_bytes);
        if (!next_logical) {
            return base::Result<pipeline::BackupSummary>::failure(next_logical.error());
        }
        auto next_stored = checked_add_wire(total.stored_bytes, backup.value().stored_bytes);
        if (!next_stored) {
            return base::Result<pipeline::BackupSummary>::failure(next_stored.error());
        }
        total.logical_bytes = next_logical.value();
        total.stored_bytes = next_stored.value();
        total.chunk_count += backup.value().chunk_count;
        total.peak_buffered_bytes =
            (std::max)(total.peak_buffered_bytes, backup.value().peak_buffered_bytes);
    }
    return base::Result<pipeline::BackupSummary>::success(total);
}

base::Result<WindowsPersonalBackupResult> backup_windows_personal_volumes_with_runtime(
    const WindowsPersonalBackupRequest& request, const base::CancellationToken& cancellation,
    ports::IProgressSink* progress, IWindowsPersonalBackupRuntime& runtime) {
    auto validation = validate_request(request);
    if (!validation) {
        return base::Result<WindowsPersonalBackupResult>::failure(validation.error());
    }
    auto prepared = runtime.prepare_sources(request.volume_guid_paths,
                                            request.exclude_page_and_hibernation_files, cancellation);
    if (!prepared) {
        return base::Result<WindowsPersonalBackupResult>::failure(prepared.error());
    }

    auto manifest = make_manifest(request, prepared.value().metadata);
    if (!manifest) {
        (void)release_snapshot(prepared.value());
        return base::Result<WindowsPersonalBackupResult>::failure(manifest.error());
    }
    auto session = runtime.create_archive(request, manifest.value());
    if (!session) {
        (void)release_snapshot(prepared.value());
        return base::Result<WindowsPersonalBackupResult>::failure(session.error());
    }
    log_runtime_info(std::string("Archive destination: ") + utf8_path(request.destination));
    log_runtime_info("Prepared VSS and raw volume sources; starting backup pipelines");
    {
        const auto hardware = std::thread::hardware_concurrency();
        const auto workers = hardware == 0 ? 4U : hardware;
        log_runtime_info(std::string("Using ") + std::to_string(workers) +
                         " worker threads for backup processing");
    }

    auto backup = run_volume_pipelines(request, prepared.value(), *session.value(), progress,
                                       cancellation);
    auto cleanup_error = release_snapshot(prepared.value());
    if (!backup) {
        return base::Result<WindowsPersonalBackupResult>::failure(backup.error());
    }
    if (cleanup_error) {
        log_runtime_info("Snapshot cleanup reported a non-fatal error after commit");
    }
    return base::Result<WindowsPersonalBackupResult>::success(
        WindowsPersonalBackupResult{backup.value(), std::move(cleanup_error)});
}

} // namespace detail

base::Result<WindowsPersonalBackupResult>
backup_windows_personal_volumes(const WindowsPersonalBackupRequest& request,
                                const base::CancellationToken& cancellation,
                                ports::IProgressSink* progress) {
    try {
        auto runtime = detail::make_windows_personal_backup_runtime();
        return detail::backup_windows_personal_volumes_with_runtime(request, cancellation, progress,
                                                                    *runtime);
    } catch (...) {
        return base::Result<WindowsPersonalBackupResult>::failure(base::Error{
            base::ErrorCode::kInternal,
            "Windows personal volume backup failed unexpectedly",
        });
    }
}

} // namespace aegra::apps::worker
