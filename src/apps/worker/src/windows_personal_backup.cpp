#include "aegra/apps/worker/windows_personal_backup.h"

#include "windows_personal_backup_runtime.h"
#include "worker_task_log.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/contracts/service_control.h"
#include "aegra/format/manifest.h"
#include "aegra/format/personal_archive.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
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
    disk.raw_layout.mbr_sector = layout.raw_layout.mbr_sector;
    disk.raw_layout.gpt_primary_header = layout.raw_layout.gpt_primary_header;
    disk.raw_layout.gpt_partition_entries = layout.raw_layout.gpt_partition_entries;
    disk.raw_layout.gpt_backup_header = layout.raw_layout.gpt_backup_header;
    disk.raw_layout.gpt_backup_entries = layout.raw_layout.gpt_backup_entries;
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
        volume.free_size_known = metadata.free_size_known;
        if (volume.free_size_known) {
            volume.free_size = metadata.free_size_bytes > metadata.logical_size_bytes
                                   ? metadata.logical_size_bytes
                                   : metadata.free_size_bytes;
        } else {
            volume.free_size = 0;
        }
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

[[nodiscard]] std::string_view backup_stage_hint(const base::Error& error) noexcept {
    if (error.message.find("VSS") != std::string::npos) {
        return "Check VSS service health and volume snapshot support";
    }
    if (error.code == base::ErrorCode::kInsufficientSpace) {
        return "Free space on the repository volume or choose another destination";
    }
    if (error.message.find("parent") != std::string::npos ||
        error.message.find("archive chunk") != std::string::npos ||
        error.message.find("archive volume") != std::string::npos) {
        return "Parent archive may be incomplete or unreadable; verify the parent .bkf "
               "and sidecar, or run a new full backup";
    }
    return {};
}

base::Result<pipeline::BackupSummary>
run_volume_pipelines(const WindowsPersonalBackupRequest& request, PreparedVolumeSources& prepared,
                     ports::IBackupSession& session, ports::IProgressSink* progress,
                     const base::CancellationToken& cancellation) {
    ScopedStage stage(WorkerTaskLog::active(), "backup_pipeline");
    pipeline::BackupSummary total;
    std::uint64_t job_logical_bytes = 0;
    for (const auto& metadata : prepared.metadata) {
        auto summed = checked_add_wire(job_logical_bytes, metadata.logical_size_bytes);
        if (!summed) {
            stage.fail(summed.error(), "sum_logical_sizes", backup_stage_hint(summed.error()));
            return base::Result<pipeline::BackupSummary>::failure(summed.error());
        }
        job_logical_bytes = summed.value();
    }
    stage.note_bytes("job_logical_size", job_logical_bytes);
    stage.note_u64("volume_count", prepared.sources.size());
    for (std::size_t index = 0; index < prepared.sources.size(); ++index) {
        const auto& metadata = prepared.metadata[index];
        if (auto* log = WorkerTaskLog::active(); log != nullptr) {
            log->field("volume_index", std::to_string(index));
            log->field("volume_path", utf8_path(metadata.volume_guid_path));
            log->field_bytes("volume_size", metadata.logical_size_bytes);
            log->field("read_mode", metadata.vss_used ? "vss" : "raw");
        }
        pipeline::BackupPlan plan{request.job_id, request.trace_id, request.chunk_size_bytes,
                                  request.memory_budget_bytes};
        plan.source_index = static_cast<std::uint32_t>(index);
        plan.commit_mode = index + 1 == prepared.sources.size()
                               ? pipeline::BackupCommitMode::kCommit
                               : pipeline::BackupCommitMode::kDefer;
        plan.progress_total_logical_bytes = job_logical_bytes;
        plan.progress_base_processed_bytes = total.logical_bytes;
        plan.progress_base_stored_bytes = total.stored_bytes;
        auto window_end = checked_add_wire(total.logical_bytes, metadata.logical_size_bytes);
        if (!window_end) {
            stage.fail(window_end.error(), "progress_window", backup_stage_hint(window_end.error()));
            return base::Result<pipeline::BackupSummary>::failure(window_end.error());
        }
        if (window_end.value() > job_logical_bytes) {
            const base::Error error{base::ErrorCode::kInternal,
                                    "volume progress window exceeds precomputed job logical total"};
            stage.fail(error, "progress_window", {});
            return base::Result<pipeline::BackupSummary>::failure(error);
        }
        pipeline::BackupPipeline pipeline(*prepared.sources[index], session, progress);
        auto backup = pipeline.run(plan, cancellation);
        if (!backup) {
            stage.fail(backup.error(), "pipeline_volume", backup_stage_hint(backup.error()));
            return base::Result<pipeline::BackupSummary>::failure(backup.error());
        }
        auto next_logical = checked_add_wire(total.logical_bytes, backup.value().logical_bytes);
        if (!next_logical) {
            stage.fail(next_logical.error(), "accumulate_logical", {});
            return base::Result<pipeline::BackupSummary>::failure(next_logical.error());
        }
        auto next_stored = checked_add_wire(total.stored_bytes, backup.value().stored_bytes);
        if (!next_stored) {
            stage.fail(next_stored.error(), "accumulate_stored", {});
            return base::Result<pipeline::BackupSummary>::failure(next_stored.error());
        }
        total.logical_bytes = next_logical.value();
        total.stored_bytes = next_stored.value();
        total.chunk_count += backup.value().chunk_count;
        total.peak_buffered_bytes =
            (std::max)(total.peak_buffered_bytes, backup.value().peak_buffered_bytes);
        total.producer_read_microseconds += backup.value().producer_read_microseconds;
        total.producer_payload_allocate_microseconds +=
            backup.value().producer_payload_allocate_microseconds;
        total.producer_buffer_wait_microseconds +=
            backup.value().producer_buffer_wait_microseconds;
        total.producer_extent_describe_microseconds +=
            backup.value().producer_extent_describe_microseconds;
        total.producer_source_read_microseconds +=
            backup.value().producer_source_read_microseconds;
        total.producer_source_read_bytes += backup.value().producer_source_read_bytes;
        total.producer_free_bytes += backup.value().producer_free_bytes;
        total.producer_extent_describe_calls +=
            backup.value().producer_extent_describe_calls;
        total.producer_source_read_calls += backup.value().producer_source_read_calls;
        total.producer_queue_wait_microseconds +=
            backup.value().producer_queue_wait_microseconds;
        total.consumer_queue_wait_microseconds +=
            backup.value().consumer_queue_wait_microseconds;
        total.consumer_write_microseconds += backup.value().consumer_write_microseconds;
        total.consumer_progress_microseconds += backup.value().consumer_progress_microseconds;
    }
    stage.note_bytes("logical_bytes", total.logical_bytes);
    // Pipeline "stored" tracks descriptor.stored_size (volume stage-2 == logical), not .bkf wire.
    // True archive size is projected from committed Footer / part files after commit (O3).
    stage.note_u64("chunks", total.chunk_count);
    stage.note_bytes("peak_buffer", total.peak_buffered_bytes);
    stage.note_u64("pipeline_producer_read_us", total.producer_read_microseconds);
    stage.note_u64("pipeline_producer_payload_allocate_us",
                   total.producer_payload_allocate_microseconds);
    stage.note_u64("pipeline_producer_buffer_wait_us",
                   total.producer_buffer_wait_microseconds);
    stage.note_u64("pipeline_producer_extent_describe_us",
                   total.producer_extent_describe_microseconds);
    stage.note_u64("pipeline_producer_source_read_us",
                   total.producer_source_read_microseconds);
    stage.note_bytes("pipeline_producer_source_read_bytes", total.producer_source_read_bytes);
    stage.note_bytes("pipeline_producer_free_bytes", total.producer_free_bytes);
    stage.note_u64("pipeline_producer_extent_describe_calls",
                   total.producer_extent_describe_calls);
    stage.note_u64("pipeline_producer_source_read_calls", total.producer_source_read_calls);
    stage.note_u64("pipeline_producer_queue_wait_us", total.producer_queue_wait_microseconds);
    stage.note_u64("pipeline_consumer_queue_wait_us", total.consumer_queue_wait_microseconds);
    stage.note_u64("pipeline_consumer_session_write_us", total.consumer_write_microseconds);
    stage.note_u64("pipeline_consumer_progress_us", total.consumer_progress_microseconds);
    if (const auto* archive_session =
            dynamic_cast<const adapters::personal_archive::PersonalArchiveSession*>(&session)) {
        const auto metrics = archive_session->write_metrics();
        stage.note_u64("archive_prepare_us", metrics.prepare_microseconds);
        stage.note_u64("archive_persist_us", metrics.persist_microseconds);
        stage.note_u64("archive_commit_us", metrics.commit_microseconds);
        stage.note_u64("archive_prepare_hash_us", metrics.prepare_hash_microseconds);
        stage.note_u64("archive_prepare_compress_us", metrics.prepare_compress_microseconds);
        stage.note_u64("archive_write_file_us", metrics.write_file_microseconds);
        stage.note_bytes("archive_write_file_bytes", metrics.write_file_bytes);
        stage.note_u64("archive_write_file_calls", metrics.write_file_calls);
    }
    return base::Result<pipeline::BackupSummary>::success(total);
}

[[nodiscard]] std::filesystem::path archive_part_path(const std::filesystem::path& destination,
                                                     const std::uint32_t part_index) {
    if (part_index == 0) {
        return destination;
    }
    std::ostringstream suffix;
    suffix << '.' << std::setw(3) << std::setfill('0') << part_index;
    auto result = destination;
    result += suffix.str();
    return result;
}

[[nodiscard]] base::Result<std::filesystem::path>
resolve_final_archive_part(const std::filesystem::path& destination) {
    namespace archive = format::personal_archive;
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(destination, filesystem_error) || filesystem_error) {
        return base::Result<std::filesystem::path>::failure(
            base::Error{base::ErrorCode::kCorruptData, "committed archive main part is missing"});
    }
    const auto file_size = std::filesystem::file_size(destination, filesystem_error);
    if (filesystem_error || file_size < archive::kBackupHeaderSize) {
        return base::Result<std::filesystem::path>::failure(
            base::Error{base::ErrorCode::kCorruptData, "committed archive header is missing"});
    }
    std::ifstream input(destination, std::ios::binary);
    if (!input) {
        return base::Result<std::filesystem::path>::failure(
            base::Error{base::ErrorCode::kIoFailure, "failed to open committed archive"});
    }
    std::array<std::byte, archive::kBackupHeaderSize> header_bytes{};
    input.read(reinterpret_cast<char*>(header_bytes.data()),
               static_cast<std::streamsize>(header_bytes.size()));
    if (!input) {
        return base::Result<std::filesystem::path>::failure(
            base::Error{base::ErrorCode::kIoFailure, "failed to read committed archive header"});
    }
    auto header = archive::decode_backup_header(header_bytes);
    if (!header) {
        return base::Result<std::filesystem::path>::failure(header.error());
    }
    const bool split = (header.value().flags & archive::kBackupFlagSplit) != 0;
    if (!split) {
        return base::Result<std::filesystem::path>::success(destination);
    }
    // Matches repository catalog split bound; worker does not depend on personal_repository.
    constexpr std::uint32_t kMaximumSplitPartCount = 1'000;
    const auto declared = header.value().split_part_count;
    const auto maximum = declared != 0 ? declared : kMaximumSplitPartCount;
    std::filesystem::path final_part = destination;
    for (std::uint32_t index = 1; index < maximum; ++index) {
        const auto candidate = archive_part_path(destination, index);
        if (!std::filesystem::is_regular_file(candidate, filesystem_error) || filesystem_error) {
            if (declared != 0) {
                return base::Result<std::filesystem::path>::failure(base::Error{
                    base::ErrorCode::kCorruptData, "committed archive split part is missing"});
            }
            break;
        }
        final_part = candidate;
    }
    return base::Result<std::filesystem::path>::success(std::move(final_part));
}

struct FooterCommitMetrics final {
    /// Sum of all committed archive part file sizes on disk.
    std::uint64_t archive_file_bytes{0};
    /// Footer total_payload_size (chunk payloads only).
    std::uint64_t total_payload_bytes{0};
    std::uint64_t deduplicated_block_count{0};
    std::uint64_t deduplicated_logical_bytes{0};
};

[[nodiscard]] base::Result<std::uint64_t>
sum_archive_part_file_bytes(const std::filesystem::path& destination,
                            const std::filesystem::path& final_part) {
    std::error_code filesystem_error;
    std::uint64_t total = 0;
    for (std::uint32_t index = 0;; ++index) {
        const auto part = archive_part_path(destination, index);
        if (!std::filesystem::is_regular_file(part, filesystem_error) || filesystem_error) {
            if (index == 0) {
                return base::Result<std::uint64_t>::failure(base::Error{
                    base::ErrorCode::kCorruptData, "committed archive main part is missing"});
            }
            break;
        }
        const auto size = std::filesystem::file_size(part, filesystem_error);
        if (filesystem_error) {
            return base::Result<std::uint64_t>::failure(
                base::Error{base::ErrorCode::kIoFailure, "failed to size committed archive part"});
        }
        if (total > (std::numeric_limits<std::uint64_t>::max)() - size) {
            return base::Result<std::uint64_t>::failure(
                base::Error{base::ErrorCode::kInvalidArgument, "archive part size sum overflows"});
        }
        total += size;
        if (part == final_part) {
            break;
        }
    }
    return base::Result<std::uint64_t>::success(total);
}

[[nodiscard]] base::Result<FooterCommitMetrics>
read_committed_footer_metrics(const std::filesystem::path& destination) {
    namespace archive = format::personal_archive;
    auto final_part = resolve_final_archive_part(destination);
    if (!final_part) {
        return base::Result<FooterCommitMetrics>::failure(final_part.error());
    }
    std::error_code filesystem_error;
    const auto file_size = std::filesystem::file_size(final_part.value(), filesystem_error);
    if (filesystem_error || file_size < archive::kBackupFooterSize) {
        return base::Result<FooterCommitMetrics>::failure(
            base::Error{base::ErrorCode::kCorruptData, "committed archive footer is missing"});
    }
    std::ifstream input(final_part.value(), std::ios::binary);
    if (!input) {
        return base::Result<FooterCommitMetrics>::failure(
            base::Error{base::ErrorCode::kIoFailure, "failed to open committed archive footer"});
    }
    input.seekg(static_cast<std::streamoff>(file_size - archive::kBackupFooterSize),
                std::ios::beg);
    std::array<std::byte, archive::kBackupFooterSize> footer_bytes{};
    input.read(reinterpret_cast<char*>(footer_bytes.data()),
               static_cast<std::streamsize>(footer_bytes.size()));
    if (!input) {
        return base::Result<FooterCommitMetrics>::failure(
            base::Error{base::ErrorCode::kIoFailure, "failed to read committed archive footer"});
    }
    auto footer = archive::decode_backup_footer(footer_bytes);
    if (!footer) {
        return base::Result<FooterCommitMetrics>::failure(footer.error());
    }
    if (footer.value().part_file_size != file_size) {
        return base::Result<FooterCommitMetrics>::failure(
            base::Error{base::ErrorCode::kCorruptData, "committed archive footer size mismatch"});
    }
    auto wire = sum_archive_part_file_bytes(destination, final_part.value());
    if (!wire) {
        return base::Result<FooterCommitMetrics>::failure(wire.error());
    }
    FooterCommitMetrics metrics;
    metrics.archive_file_bytes = wire.value();
    metrics.total_payload_bytes = footer.value().total_payload_size;
    metrics.deduplicated_block_count = footer.value().deduplicated_block_count;
    metrics.deduplicated_logical_bytes = footer.value().deduplicated_logical_bytes;
    return base::Result<FooterCommitMetrics>::success(metrics);
}

base::Result<WindowsPersonalBackupResult> backup_windows_personal_volumes_with_runtime(
    const WindowsPersonalBackupRequest& request, const base::CancellationToken& cancellation,
    ports::IProgressSink* progress, IWindowsPersonalBackupRuntime& runtime) {
    auto validation = validate_request(request);
    if (!validation) {
        return base::Result<WindowsPersonalBackupResult>::failure(validation.error());
    }

    PreparedVolumeSources prepared_sources;
    {
        ScopedStage stage(WorkerTaskLog::active(), "prepare_sources");
        stage.note_u64("volume_count", request.volume_guid_paths.size());
        stage.note_bool("exclude_page_and_hibernation_files",
                        request.exclude_page_and_hibernation_files);
        auto prepared = runtime.prepare_sources(request.volume_guid_paths,
                                                request.exclude_page_and_hibernation_files,
                                                request.block_size_bytes, cancellation);
        if (!prepared) {
            stage.fail(prepared.error(), "open_volume_or_vss", backup_stage_hint(prepared.error()));
            return base::Result<WindowsPersonalBackupResult>::failure(prepared.error());
        }
        prepared_sources = std::move(prepared).value();
        std::size_t vss_count = 0;
        for (const auto& metadata : prepared_sources.metadata) {
            if (metadata.vss_used) {
                ++vss_count;
            }
        }
        stage.note_u64("vss_volumes", vss_count);
        stage.note_u64("raw_volumes", prepared_sources.metadata.size() - vss_count);
    }

    auto manifest = make_manifest(request, prepared_sources.metadata);
    if (!manifest) {
        (void)release_snapshot(prepared_sources);
        return base::Result<WindowsPersonalBackupResult>::failure(manifest.error());
    }

    std::unique_ptr<ports::IBackupSession> session;
    {
        ScopedStage stage(WorkerTaskLog::active(), "create_archive");
        stage.note("destination", utf8_path(request.destination));
        stage.note_bool("encryption_enabled", request.encryption_enabled);
        if (!request.parent_source.empty()) {
            stage.note("parent_archive", utf8_path(request.parent_source));
        }
        auto opened = runtime.create_archive(request, manifest.value());
        if (!opened) {
            (void)release_snapshot(prepared_sources);
            stage.fail(opened.error(), "create_session", backup_stage_hint(opened.error()));
            return base::Result<WindowsPersonalBackupResult>::failure(opened.error());
        }
        session = std::move(opened).value();
        const auto hardware = std::thread::hardware_concurrency();
        stage.note_u64("worker_threads", hardware == 0 ? 4U : hardware);
    }

    auto backup =
        run_volume_pipelines(request, prepared_sources, *session, progress, cancellation);
    auto cleanup_error = release_snapshot(prepared_sources);
    if (!backup) {
        return base::Result<WindowsPersonalBackupResult>::failure(backup.error());
    }
    // Session owns the archive until destruction; release it before reading the committed Footer.
    session.reset();
    auto footer_metrics = read_committed_footer_metrics(request.destination);
    if (!footer_metrics) {
        return base::Result<WindowsPersonalBackupResult>::failure(footer_metrics.error());
    }
    if (cleanup_error) {
        if (auto* log = WorkerTaskLog::active(); log != nullptr) {
            log->warn("Snapshot cleanup reported a non-fatal error after commit");
            log->field("cleanup_error", cleanup_error->message);
        }
    }
    WindowsPersonalBackupResult result;
    result.backup = std::move(backup).value();
    // O3: TaskResult.stored_bytes / BackupSummary.stored_bytes = on-disk archive wire size.
    result.backup.stored_bytes = footer_metrics.value().archive_file_bytes;
    result.archive_file_bytes = footer_metrics.value().archive_file_bytes;
    result.total_payload_bytes = footer_metrics.value().total_payload_bytes;
    result.deduplicated_block_count = footer_metrics.value().deduplicated_block_count;
    result.deduplicated_logical_bytes = footer_metrics.value().deduplicated_logical_bytes;
    result.snapshot_cleanup_error = std::move(cleanup_error);
    return base::Result<WindowsPersonalBackupResult>::success(std::move(result));
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
