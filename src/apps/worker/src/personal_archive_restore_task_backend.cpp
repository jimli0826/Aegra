#include "personal_archive_restore_task_backend.h"

#include "worker_task_log.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/pipeline/restore_pipeline.h"
#include "aegra/ports/block_io.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::worker::detail {
namespace {

class OffsetBlockSink final : public ports::IBlockSink {
  public:
    OffsetBlockSink(ports::IBlockSink& inner, const std::uint64_t base_offset,
                    const std::uint64_t logical_capacity)
        : inner_(inner), base_offset_(base_offset), logical_capacity_(logical_capacity) {}

    [[nodiscard]] std::uint64_t capacity_bytes() const noexcept override {
        return logical_capacity_;
    }

    [[nodiscard]] base::Result<void> write(const std::uint64_t offset,
                                           const std::span<const std::byte> source,
                                           const base::CancellationToken cancellation) override {
        if (offset > logical_capacity_ || source.size() > logical_capacity_ - offset) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "offset sink write range is invalid"});
        }
        if (base_offset_ > (std::numeric_limits<std::uint64_t>::max)() - offset) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "offset sink write overflows"});
        }
        return inner_.write(base_offset_ + offset, source, cancellation);
    }

    [[nodiscard]] base::Result<void> flush(const base::CancellationToken cancellation) override {
        // Disk-level flush is performed once after all volumes.
        static_cast<void>(cancellation);
        return base::Result<void>::success();
    }

  private:
    ports::IBlockSink& inner_;
    std::uint64_t base_offset_{0};
    std::uint64_t logical_capacity_{0};
};

struct DiskVolumeTarget final {
    std::uint32_t volume_index{0};
    std::uint64_t physical_offset{0};
    std::uint64_t volume_size{0};
};

[[nodiscard]] std::string partition_style_name(const format::PartitionStyle style) {
    switch (style) {
    case format::PartitionStyle::kMbr:
        return "MBR";
    case format::PartitionStyle::kGpt:
        return "GPT";
    case format::PartitionStyle::kRaw:
        return "RAW";
    }
    return "RAW";
}

[[nodiscard]] base::Result<const format::Disk*>
find_source_disk(const format::Manifest& manifest, const std::uint32_t source_disk_number) {
    for (const auto& disk : manifest.disks) {
        if (disk.disk_number == source_disk_number) {
            return base::Result<const format::Disk*>::success(&disk);
        }
    }
    return base::Result<const format::Disk*>::failure(
        {base::ErrorCode::kNotFound, "source disk is not present in archive manifest"});
}

[[nodiscard]] std::string_view stage_hint(const base::Error& error) noexcept {
    const auto& message = error.message;
    if (message.find("sharing violation") != std::string::npos ||
        message.find("Win32 error 32") != std::string::npos) {
        return "Close Explorer windows and apps using the target volume, then retry";
    }
    if (message.find("in use") != std::string::npos) {
        return "Close open handles on the target, then retry";
    }
    if (message.find("insufficient") != std::string::npos ||
        error.code == base::ErrorCode::kInsufficientSpace) {
        return "Choose a larger target volume or disk";
    }
    if (error.code == base::ErrorCode::kCorruptData) {
        return "Re-backup the source or pick another recovery point";
    }
    return {};
}

[[nodiscard]] std::string_view sink_fail_step(const std::string_view message) noexcept {
    if (message.find("CreateFileW") != std::string_view::npos) {
        return "CreateFileW";
    }
    if (message.find("FSCTL_LOCK_VOLUME") != std::string_view::npos) {
        return "FSCTL_LOCK_VOLUME";
    }
    if (message.find("FSCTL_DISMOUNT_VOLUME") != std::string_view::npos) {
        return "FSCTL_DISMOUNT_VOLUME";
    }
    if (message.find("FSCTL_ALLOW_EXTENDED_DASD_IO") != std::string_view::npos) {
        return "FSCTL_ALLOW_EXTENDED_DASD_IO";
    }
    if (message.find("IOCTL_DISK_GET_LENGTH_INFO") != std::string_view::npos ||
        message.find("volume length") != std::string_view::npos) {
        return "IOCTL_DISK_GET_LENGTH_INFO";
    }
    if (message.find("protected source") != std::string_view::npos ||
        message.find("GetVolumePathNameW") != std::string_view::npos) {
        return "validate_protected_sources";
    }
    if (message.find("system volume") != std::string_view::npos ||
        message.find("system disk") != std::string_view::npos) {
        return "reject_system_target";
    }
    return "open_sink";
}

[[nodiscard]] base::Result<pipeline::RestoreSummary>
fail_restore(ScopedStage& stage, const base::Error& error, const std::string_view step = {}) {
    const auto resolved_step = step.empty() ? sink_fail_step(error.message) : step;
    stage.fail(error, resolved_step, stage_hint(error));
    return base::Result<pipeline::RestoreSummary>::failure(error);
}

[[nodiscard]] base::Result<std::vector<DiskVolumeTarget>>
plan_disk_volumes(const format::Manifest& manifest, const std::uint32_t source_disk_number) {
    std::vector<DiskVolumeTarget> targets;
    for (const auto& volume : manifest.volumes) {
        std::optional<std::uint64_t> physical_offset;
        for (const auto& extent : volume.extents) {
            if (extent.disk_number != source_disk_number) {
                continue;
            }
            if (extent.volume_offset != 0) {
                return base::Result<std::vector<DiskVolumeTarget>>::failure(
                    {base::ErrorCode::kInvalidArgument,
                     "disk restore step-1 requires single contiguous volume extents"});
            }
            if (physical_offset.has_value()) {
                return base::Result<std::vector<DiskVolumeTarget>>::failure(
                    {base::ErrorCode::kInvalidArgument,
                     "disk restore step-1 requires one extent per volume on the source disk"});
            }
            physical_offset = extent.physical_offset;
        }
        if (!physical_offset.has_value()) {
            continue;
        }
        // Reject multi-disk volumes that also have extents elsewhere.
        for (const auto& extent : volume.extents) {
            if (extent.disk_number != source_disk_number) {
                return base::Result<std::vector<DiskVolumeTarget>>::failure(
                    {base::ErrorCode::kInvalidArgument,
                     "disk restore step-1 rejects multi-disk volumes"});
            }
        }
        targets.push_back({volume.volume_index, physical_offset.value(), volume.total_size});
    }
    if (targets.empty()) {
        return base::Result<std::vector<DiskVolumeTarget>>::failure(
            {base::ErrorCode::kInvalidArgument, "source disk has no backed-up volumes"});
    }
    std::sort(targets.begin(), targets.end(),
              [](const DiskVolumeTarget& left, const DiskVolumeTarget& right) {
                  return left.physical_offset < right.physical_offset;
              });
    return base::Result<std::vector<DiskVolumeTarget>>::success(std::move(targets));
}

[[nodiscard]] adapters::windows_disk::WindowsRawDiskLayout
to_windows_raw_layout(const format::RawDiskLayout& layout) {
    adapters::windows_disk::WindowsRawDiskLayout result;
    result.mbr_sector = layout.mbr_sector;
    result.gpt_primary_header = layout.gpt_primary_header;
    result.gpt_partition_entries = layout.gpt_partition_entries;
    result.gpt_backup_header = layout.gpt_backup_header;
    result.gpt_backup_entries = layout.gpt_backup_entries;
    return result;
}

[[nodiscard]] adapters::personal_archive::ArchiveChainOpenRequest
make_chain_open_request(const PersonalArchiveRestoreBackendRequest& request,
                        std::vector<std::filesystem::path>& protected_sources) {
    adapters::personal_archive::ArchiveChainOpenRequest open_request;
    open_request.maximum_chain_depth = request.maximum_chain_depth;
    open_request.layers.reserve(request.layers.size());
    protected_sources.reserve(request.layers.size());
    for (const auto& layer : request.layers) {
        adapters::personal_archive::ArchiveOpenRequest layer_request;
        layer_request.source = layer.source;
        layer_request.password = layer.password;
        layer_request.maximum_chunk_payload_size = request.maximum_chunk_size;
        layer_request.maximum_chunk_logical_size = request.maximum_chunk_size;
        open_request.layers.push_back(std::move(layer_request));
        protected_sources.push_back(layer.source);
    }
    return open_request;
}

using ChainReaderPtr = std::unique_ptr<adapters::personal_archive::PersonalArchiveChainReader>;

[[nodiscard]] base::Result<ChainReaderPtr>
open_chain_reader_stage(const adapters::personal_archive::ArchiveChainOpenRequest& open_request,
                        const std::size_t layer_count) {
    ScopedStage stage(WorkerTaskLog::active(), "open_chain_reader");
    stage.note_u64("layer_count", layer_count);
    auto opened = adapters::personal_archive::PersonalArchiveChainReader::open(open_request);
    if (!opened) {
        stage.fail(opened.error(), "open_archive_chain", stage_hint(opened.error()));
        return base::Result<ChainReaderPtr>::failure(opened.error());
    }
    stage.note_u64("volumes_in_manifest", opened.value()->manifest().volumes.size());
    stage.note_u64("disks_in_manifest", opened.value()->manifest().disks.size());
    return base::Result<ChainReaderPtr>::success(std::move(opened).value());
}

struct DiskRestorePlan final {
    const format::Disk* source_disk{nullptr};
    std::vector<DiskVolumeTarget> targets;
};

[[nodiscard]] base::Result<DiskRestorePlan>
plan_disk_restore_stage(const format::Manifest& manifest, const std::uint32_t source_disk_number) {
    ScopedStage stage(WorkerTaskLog::active(), "plan_disk_volumes");
    stage.note_u64("source_disk_number", source_disk_number);
    auto found = find_source_disk(manifest, source_disk_number);
    if (!found) {
        stage.fail(found.error(), "find_source_disk", stage_hint(found.error()));
        return base::Result<DiskRestorePlan>::failure(found.error());
    }
    const auto* source_disk = found.value();
    if (source_disk->disk_size == 0) {
        const base::Error error{base::ErrorCode::kCorruptData,
                                "source disk size is missing from manifest"};
        stage.fail(error, "validate_disk_size", stage_hint(error));
        return base::Result<DiskRestorePlan>::failure(error);
    }
    const auto has_raw_layout = !source_disk->raw_layout.mbr_sector.empty() ||
                                source_disk->partition_style == format::PartitionStyle::kRaw;
    if (!has_raw_layout) {
        const base::Error error{base::ErrorCode::kCorruptData,
                                "archive lacks raw_layout; re-backup the disk before disk restore"};
        stage.fail(error, "require_raw_layout", stage_hint(error));
        return base::Result<DiskRestorePlan>::failure(error);
    }
    auto planned = plan_disk_volumes(manifest, source_disk_number);
    if (!planned) {
        stage.fail(planned.error(), "plan_extents", stage_hint(planned.error()));
        return base::Result<DiskRestorePlan>::failure(planned.error());
    }
    stage.note_bytes("source_disk_size", source_disk->disk_size);
    stage.note_u64("bytes_per_sector", source_disk->bytes_per_sector);
    stage.note("partition_style", partition_style_name(source_disk->partition_style));
    stage.note_u64("volume_count", planned.value().size());
    return base::Result<DiskRestorePlan>::success(
        DiskRestorePlan{source_disk, std::move(planned).value()});
}

[[nodiscard]] base::Result<std::unique_ptr<adapters::windows_disk::WindowsBlockSink>>
open_disk_sink_stage(const PersonalArchiveRestoreBackendRequest& request,
                     const format::Disk& source_disk,
                     std::vector<std::filesystem::path> protected_sources) {
    ScopedStage stage(WorkerTaskLog::active(), "open_disk_sink");
    stage.note("kind", "physical_disk");
    stage.note("target", path_display(request.target));
    adapters::windows_disk::WindowsBlockSinkOpenRequest sink_request;
    sink_request.path = request.target;
    sink_request.kind = adapters::windows_disk::WindowsBlockSinkKind::kPhysicalDisk;
    sink_request.protected_sources = std::move(protected_sources);
    sink_request.minimum_capacity_bytes = source_disk.disk_size;
    sink_request.expected_bytes_per_sector = source_disk.bytes_per_sector;
    auto opened = adapters::windows_disk::WindowsBlockSink::open(sink_request);
    if (!opened) {
        stage.fail(opened.error(), sink_fail_step(opened.error().message),
                   stage_hint(opened.error()));
        return base::Result<std::unique_ptr<adapters::windows_disk::WindowsBlockSink>>::failure(
            opened.error());
    }
    stage.note_bytes("capacity", opened.value()->capacity_bytes());
    return opened;
}

[[nodiscard]] base::Result<pipeline::RestoreSummary>
restore_disk_volumes_stage(ports::IRecoveryPointReader& reader, const format::Manifest& manifest,
                           ports::IBlockSink& disk_sink,
                           const std::vector<DiskVolumeTarget>& targets,
                           const PersonalArchiveRestoreBackendRequest& request,
                           const base::CancellationToken& cancellation) {
    ScopedStage stage(WorkerTaskLog::active(), "restore_pipeline");
    stage.note_u64("volume_count", targets.size());
    pipeline::RestoreSummary summary;
    for (const auto& target : targets) {
        if (cancellation.stop_requested()) {
            return fail_restore(stage, {base::ErrorCode::kCancelled, "disk restore cancelled"},
                                "cancel");
        }
        if (auto* log = WorkerTaskLog::active(); log != nullptr) {
            log->field("volume_index", std::to_string(target.volume_index));
            log->field_bytes("volume_size", target.volume_size);
            log->field_u64("physical_offset", target.physical_offset);
        }
        auto volume_reader = adapters::personal_archive::PersonalArchiveVolumeReader::open(
            reader, manifest, target.volume_index);
        if (!volume_reader) {
            return fail_restore(stage, volume_reader.error(), "open_volume_reader");
        }
        OffsetBlockSink offset_sink(disk_sink, target.physical_offset, target.volume_size);
        pipeline::RestorePipeline restore(*volume_reader.value(), offset_sink, request.progress);
        auto volume_summary = restore.run(request.plan, cancellation);
        if (!volume_summary) {
            return fail_restore(stage, volume_summary.error(), "pipeline_volume");
        }
        summary.restored_bytes += volume_summary.value().restored_bytes;
        summary.chunk_count += volume_summary.value().chunk_count;
        summary.peak_buffered_bytes =
            (std::max)(summary.peak_buffered_bytes, volume_summary.value().peak_buffered_bytes);
    }
    auto flushed = disk_sink.flush(cancellation);
    if (!flushed) {
        return fail_restore(stage, flushed.error(), "flush");
    }
    stage.note_bytes("restored_bytes", summary.restored_bytes);
    stage.note_u64("chunks", summary.chunk_count);
    return base::Result<pipeline::RestoreSummary>::success(summary);
}

[[nodiscard]] base::Result<void>
rebuild_partition_table_stage(const PersonalArchiveRestoreBackendRequest& request,
                              const format::Disk& source_disk, const std::uint32_t target_disk) {
    ScopedStage stage(WorkerTaskLog::active(), "rebuild_partition_table");
    const auto style = partition_style_name(source_disk.partition_style);
    stage.note("partition_style", style);
    stage.note("preserve_disk_signature", request.preserve_disk_signature ? "true" : "false");
    auto layout = adapters::windows_disk::apply_disk_signature_policy(
        to_windows_raw_layout(source_disk.raw_layout), request.preserve_disk_signature, style);
    if (!layout) {
        stage.fail(layout.error(), "signature_policy", stage_hint(layout.error()));
        return base::Result<void>::failure(layout.error());
    }
    auto rebuilt = adapters::windows_disk::rebuild_partition_table_from_raw_layout(
        target_disk, source_disk.bytes_per_sector, source_disk.disk_size, layout.value(), style);
    if (!rebuilt) {
        stage.fail(rebuilt.error(), "write_mbr_gpt", stage_hint(rebuilt.error()));
        return base::Result<void>::failure(rebuilt.error());
    }
    if (request.bring_target_online) {
        auto online = adapters::windows_disk::bring_target_disk_online(target_disk);
        if (!online) {
            stage.fail(online.error(), "bring_online", stage_hint(online.error()));
            return base::Result<void>::failure(online.error());
        }
        stage.note("bring_online", "ok");
    }
    if (request.auto_expand_last_partition) {
        auto expanded = adapters::windows_disk::expand_last_data_partition_on_disk(
            target_disk, source_disk.disk_size, source_disk.bytes_per_sector, style);
        if (!expanded) {
            stage.fail(expanded.error(), "expand_last_partition", stage_hint(expanded.error()));
            return base::Result<void>::failure(expanded.error());
        }
        stage.note("auto_expand", "ok");
    }
    return base::Result<void>::success();
}

class PersonalArchiveRestoreTaskBackend final : public IPersonalArchiveRestoreTaskBackend {
  public:
    base::Result<pipeline::RestoreSummary>
    run(const PersonalArchiveRestoreBackendRequest& request,
        const base::CancellationToken& cancellation) override {
        if (request.disk_restore) {
            return run_disk_restore(request, cancellation);
        }
        return run_volume_restore(request, cancellation);
    }

  private:
    [[nodiscard]] static base::Result<pipeline::RestoreSummary>
    run_volume_restore(const PersonalArchiveRestoreBackendRequest& request,
                       const base::CancellationToken& cancellation) {
        if (request.layers.empty()) {
            return base::Result<pipeline::RestoreSummary>::failure(
                {base::ErrorCode::kInvalidArgument, "volume restore requires at least one archive"});
        }
        std::vector<std::filesystem::path> protected_sources;
        auto open_request = make_chain_open_request(request, protected_sources);
        auto reader = open_chain_reader_stage(open_request, request.layers.size());
        if (!reader) {
            return base::Result<pipeline::RestoreSummary>::failure(reader.error());
        }

        const auto& manifest = reader.value()->manifest();
        std::unique_ptr<adapters::personal_archive::PersonalArchiveVolumeReader> volume_reader;
        {
            ScopedStage stage(WorkerTaskLog::active(), "open_volume_reader");
            stage.note_u64("source_volume_index", request.source_volume_index);
            auto opened = adapters::personal_archive::PersonalArchiveVolumeReader::open(
                *reader.value(), manifest, request.source_volume_index);
            if (!opened) {
                return fail_restore(stage, opened.error(), "select_volume_chunks");
            }
            volume_reader = std::move(opened).value();
            stage.note_bytes("logical_size", volume_reader->logical_size_bytes());
            stage.note_u64("chunk_count", volume_reader->chunk_count());
        }

        std::unique_ptr<adapters::windows_disk::WindowsBlockSink> sink;
        {
            ScopedStage stage(WorkerTaskLog::active(), "open_volume_sink");
            stage.note("kind", "volume");
            stage.note("target", path_display(request.target));
            auto opened = adapters::windows_disk::WindowsBlockSink::open(
                {request.target, adapters::windows_disk::WindowsBlockSinkKind::kVolume,
                 std::nullopt, std::move(protected_sources)});
            if (!opened) {
                return fail_restore(stage, opened.error());
            }
            sink = std::move(opened).value();
            stage.note_bytes("capacity", sink->capacity_bytes());
            stage.note_bytes("source_logical_size", volume_reader->logical_size_bytes());
            if (volume_reader->logical_size_bytes() > sink->capacity_bytes()) {
                return fail_restore(stage,
                                    {base::ErrorCode::kInsufficientSpace,
                                     "restore target capacity is insufficient"},
                                    "capacity_preflight");
            }
            stage.note("preflight", "source_size <= target_capacity");
        }

        {
            ScopedStage stage(WorkerTaskLog::active(), "restore_pipeline");
            stage.note_u64("chunk_count", volume_reader->chunk_count());
            stage.note_bytes("logical_size", volume_reader->logical_size_bytes());
            pipeline::RestorePipeline restore(*volume_reader, *sink, request.progress);
            auto summary = restore.run(request.plan, cancellation);
            if (!summary) {
                return fail_restore(stage, summary.error(), "pipeline_run");
            }
            stage.note_bytes("restored_bytes", summary.value().restored_bytes);
            stage.note_u64("chunks", summary.value().chunk_count);
            stage.note_bytes("peak_buffer", summary.value().peak_buffered_bytes);
            return summary;
        }
    }

    [[nodiscard]] static base::Result<pipeline::RestoreSummary>
    run_disk_restore(const PersonalArchiveRestoreBackendRequest& request,
                     const base::CancellationToken& cancellation) {
        if (request.layers.empty()) {
            return base::Result<pipeline::RestoreSummary>::failure(
                {base::ErrorCode::kInvalidArgument, "disk restore requires at least one archive"});
        }
        std::vector<std::filesystem::path> protected_sources;
        auto open_request = make_chain_open_request(request, protected_sources);
        auto reader = open_chain_reader_stage(open_request, request.layers.size());
        if (!reader) {
            return base::Result<pipeline::RestoreSummary>::failure(reader.error());
        }
        auto plan = plan_disk_restore_stage(reader.value()->manifest(), request.source_disk_number);
        if (!plan) {
            return base::Result<pipeline::RestoreSummary>::failure(plan.error());
        }
        auto target_disk =
            adapters::windows_disk::WindowsBlockSink::physical_drive_number(request.target);
        if (!target_disk) {
            return base::Result<pipeline::RestoreSummary>::failure(
                {base::ErrorCode::kInvalidArgument, "disk restore target must be PhysicalDrive"});
        }
        {
            ScopedStage stage(WorkerTaskLog::active(), "prepare_target_disk");
            stage.note("target", path_display(request.target));
            stage.note_u64("physical_drive", target_disk.value());
            auto prepared = adapters::windows_disk::prepare_target_disk_for_raw_restore(
                target_disk.value(), plan.value().source_disk->disk_size,
                plan.value().source_disk->bytes_per_sector);
            if (!prepared) {
                return fail_restore(stage, prepared.error(), "delete_layout_or_capacity");
            }
        }
        auto disk_sink =
            open_disk_sink_stage(request, *plan.value().source_disk, std::move(protected_sources));
        if (!disk_sink) {
            return base::Result<pipeline::RestoreSummary>::failure(disk_sink.error());
        }
        auto summary = restore_disk_volumes_stage(
            *reader.value(), reader.value()->manifest(), *disk_sink.value(), plan.value().targets,
            request, cancellation);
        if (!summary) {
            return summary;
        }
        // Close data handle before rewriting partition table.
        disk_sink.value().reset();
        auto rebuilt =
            rebuild_partition_table_stage(request, *plan.value().source_disk, target_disk.value());
        if (!rebuilt) {
            return base::Result<pipeline::RestoreSummary>::failure(rebuilt.error());
        }
        return summary;
    }
};

} // namespace

std::unique_ptr<IPersonalArchiveRestoreTaskBackend>
make_personal_archive_restore_task_backend() {
    return std::make_unique<PersonalArchiveRestoreTaskBackend>();
}

} // namespace aegra::apps::worker::detail
