#include "personal_archive_restore_task_backend.h"

#include "personal_archive_restore_shrink.h"
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
#include <set>
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
    /// Where volume data is written on the target (after layout edits).
    std::uint64_t physical_offset{0};
    /// Logical size of backed-up volume data (source).
    std::uint64_t volume_size{0};
    /// Partition size on target (may be larger than volume_size after resize).
    std::uint64_t partition_size{0};
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
        targets.push_back({volume.volume_index, physical_offset.value(), volume.total_size,
                           volume.total_size});
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

/// EFI / MSR / Recovery / similar — always kept when rebuilding the table.
[[nodiscard]] bool is_reserved_manifest_partition(const format::Partition& partition) {
    auto gpt = partition.gpt_type_guid;
    for (char& ch : gpt) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    static constexpr std::string_view k_reserved[] = {
        "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", // EFI
        "e3c9e316-0b5c-4db8-817d-f92df00215ae", // MSR
        "de94bba4-06d1-4d40-a16a-bfd50179d6ac", // Recovery
        "5808c8aa-7e8f-42e0-85d2-e1e90434cfb3", // LDM metadata
        "af9b60a0-1431-4f62-bc68-3311714a69ad", // LDM data
    };
    for (const auto id : k_reserved) {
        if (gpt == id) {
            return true;
        }
    }
    switch (partition.mbr_type) {
    case 0x00:
    case 0x05:
    case 0x0F:
    case 0x12:
    case 0x27:
    case 0xEE:
    case 0xEF:
    case 0xDE:
        return partition.gpt_type_guid.empty();
    default:
        break;
    }
    return false;
}

/// Partition start offsets that must remain after disk restore (reserved + backed-up).
[[nodiscard]] std::vector<std::uint64_t>
keep_partition_offsets_for_disk_restore(const format::Disk& source_disk,
                                        const std::vector<DiskVolumeTarget>& targets) {
    std::set<std::uint64_t> keep;
    for (const auto& partition : source_disk.partitions) {
        if (is_reserved_manifest_partition(partition)) {
            keep.insert(partition.offset);
        }
    }
    for (const auto& target : targets) {
        for (const auto& partition : source_disk.partitions) {
            if (partition.size == 0) {
                continue;
            }
            const auto end = partition.offset + partition.size;
            if (target.physical_offset >= partition.offset && target.physical_offset < end) {
                keep.insert(partition.offset);
                break;
            }
        }
        // Fallback: keep the extent start even if partition match fails.
        keep.insert(target.physical_offset);
    }
    return std::vector<std::uint64_t>(keep.begin(), keep.end());
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
        // The base layer is consumed sequentially. Sparse overlays can revisit or skip records.
        layer_request.sequential_payload_prefetch = index == 0;
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
    stage.note_bool("base_sequential_payload_prefetch", true);
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

struct RestoreDiskVolumesArgs final {
    ports::IRecoveryPointReader* reader{nullptr};
    const format::Manifest* manifest{nullptr};
    ports::IBlockSink* disk_sink{nullptr};
    const std::vector<DiskVolumeTarget>* targets{nullptr};
    const PersonalArchiveRestoreBackendRequest* request{nullptr};
    const base::CancellationToken* cancellation{nullptr};
};

[[nodiscard]] base::Result<pipeline::RestoreSummary>
restore_disk_volumes_stage(const RestoreDiskVolumesArgs& args) {
    if (args.reader == nullptr || args.manifest == nullptr || args.disk_sink == nullptr ||
        args.targets == nullptr || args.request == nullptr || args.cancellation == nullptr) {
        return base::Result<pipeline::RestoreSummary>::failure(
            {base::ErrorCode::kInvalidArgument, "disk volume restore arguments are incomplete"});
    }
    ScopedStage stage(WorkerTaskLog::active(), "restore_pipeline");
    stage.note_u64("volume_count", args.targets->size());
    pipeline::RestoreSummary summary;
    for (const auto& target : *args.targets) {
        if (args.cancellation->stop_requested()) {
            return fail_restore(stage, {base::ErrorCode::kCancelled, "disk restore cancelled"},
                                "cancel");
        }
        if (auto* log = WorkerTaskLog::active(); log != nullptr) {
            log->field("volume_index", std::to_string(target.volume_index));
            log->field_bytes("volume_size", target.volume_size);
            log->field_u64("physical_offset", target.physical_offset);
        }
        auto volume_reader = adapters::personal_archive::PersonalArchiveVolumeReader::open(
            *args.reader, *args.manifest, target.volume_index);
        if (!volume_reader) {
            return fail_restore(stage, volume_reader.error(), "open_volume_reader");
        }
        OffsetBlockSink offset_sink(*args.disk_sink, target.physical_offset, target.volume_size);
        pipeline::RestorePipeline restore(*volume_reader.value(), offset_sink,
                                          args.request->progress);
        auto volume_summary = restore.run(args.request->plan, *args.cancellation);
        if (!volume_summary) {
            return fail_restore(stage, volume_summary.error(), "pipeline_volume");
        }
        summary.restored_bytes += volume_summary.value().restored_bytes;
        summary.disk_written_bytes += volume_summary.value().disk_written_bytes;
        summary.free_skipped_bytes += volume_summary.value().free_skipped_bytes;
        summary.free_range_count += volume_summary.value().free_range_count;
        summary.chunk_count += volume_summary.value().chunk_count;
        summary.peak_buffered_bytes =
            (std::max)(summary.peak_buffered_bytes, volume_summary.value().peak_buffered_bytes);
    }
    auto flushed = args.disk_sink->flush(*args.cancellation);
    if (!flushed) {
        return fail_restore(stage, flushed.error(), "flush");
    }
    stage.note_bytes("restored_bytes", summary.restored_bytes);
    stage.note_bytes("disk_written_bytes", summary.disk_written_bytes);
    stage.note_bytes("free_skipped_bytes", summary.free_skipped_bytes);
    stage.note_u64("free_ranges", summary.free_range_count);
    stage.note_u64("chunks", summary.chunk_count);
    return base::Result<pipeline::RestoreSummary>::success(summary);
}

[[nodiscard]] std::uint64_t align_down_bytes(const std::uint64_t value,
                                             const std::uint32_t sector) noexcept {
    const auto s = sector == 0 ? 512U : sector;
    return (value / s) * static_cast<std::uint64_t>(s);
}

[[nodiscard]] std::uint64_t align_up_bytes(const std::uint64_t value,
                                           const std::uint32_t sector) noexcept {
    const auto s = sector == 0 ? 512U : sector;
    if (value == 0) {
        return 0;
    }
    return ((value + s - 1U) / s) * static_cast<std::uint64_t>(s);
}

/// Same edit list feeds the partition table and volume writes — never diverge sizes.
[[nodiscard]] base::Result<void>
ensure_layout_covers_volume_payloads(
    const std::vector<contracts::RestorePartitionLayoutEdit>& edits,
    const std::vector<DiskVolumeTarget>& targets, const std::uint32_t sector) {
    if (edits.empty()) {
        return base::Result<void>::success();
    }
    if (edits.size() > contracts::kMaximumPartitionLayoutEdits) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "partition layout edit count exceeds limit"});
    }
    const auto tol = sector == 0 ? 512U : sector;
    for (const auto& target : targets) {
        for (const auto& edit : edits) {
            const auto delta = target.physical_offset >= edit.source_start_offset_bytes
                                   ? target.physical_offset - edit.source_start_offset_bytes
                                   : edit.source_start_offset_bytes - target.physical_offset;
            if (delta > tol) {
                continue;
            }
            const auto part_size = align_down_bytes(edit.size_bytes, sector);
            if (part_size < target.volume_size) {
                return base::Result<void>::failure(
                    {base::ErrorCode::kInvalidArgument,
                     "partition layout size is smaller than volume payload"});
            }
            break;
        }
    }
    return base::Result<void>::success();
}

/// Remap planned write offsets from *resolved* layout (same sizes as the table).
void apply_layout_edits_to_volume_targets(
    std::vector<DiskVolumeTarget>& targets,
    const std::vector<contracts::RestorePartitionLayoutEdit>& edits,
    const std::uint32_t sector) {
    if (edits.empty()) {
        return;
    }
    const auto tol = sector == 0 ? 512U : sector;
    for (auto& target : targets) {
        for (const auto& edit : edits) {
            const auto delta = target.physical_offset >= edit.source_start_offset_bytes
                                   ? target.physical_offset - edit.source_start_offset_bytes
                                   : edit.source_start_offset_bytes - target.physical_offset;
            if (delta > tol) {
                continue;
            }
            target.physical_offset = align_down_bytes(edit.target_start_offset_bytes, sector);
            target.partition_size = align_down_bytes(edit.size_bytes, sector);
            break;
        }
    }
}

[[nodiscard]] base::Result<std::vector<adapters::windows_disk::PartitionLayoutEdit>>
to_adapter_layout_edits(const std::vector<contracts::RestorePartitionLayoutEdit>& edits,
                        const std::uint32_t sector) {
    if (edits.size() > contracts::kMaximumPartitionLayoutEdits) {
        return base::Result<std::vector<adapters::windows_disk::PartitionLayoutEdit>>::failure(
            {base::ErrorCode::kInvalidArgument, "partition layout edit count exceeds limit"});
    }
    std::vector<adapters::windows_disk::PartitionLayoutEdit> out;
    out.reserve(edits.size());
    for (const auto& edit : edits) {
        auto start = align_down_bytes(edit.target_start_offset_bytes, sector);
        auto size = align_down_bytes(edit.size_bytes, sector);
        if (size == 0) {
            return base::Result<std::vector<adapters::windows_disk::PartitionLayoutEdit>>::failure(
                {base::ErrorCode::kInvalidArgument, "partition layout size is zero"});
        }
        out.push_back({edit.source_start_offset_bytes, start, size});
    }
    return base::Result<std::vector<adapters::windows_disk::PartitionLayoutEdit>>::success(
        std::move(out));
}

struct PreparedTargetPartitionTable final {
    std::vector<contracts::RestorePartitionLayoutEdit> resolved_edits;
    adapters::windows_disk::WindowsRawDiskLayout layout;
    std::string partition_style;
};

struct PrepareTargetPartitionTableArgs final {
    const PersonalArchiveRestoreBackendRequest* request{nullptr};
    const format::Disk* source_disk{nullptr};
    const std::vector<DiskVolumeTarget>* targets{nullptr};
    std::uint64_t target_disk_size_bytes{0};
};

/// In-memory Target layout only (signature / filter / validate / resolve / apply).
/// No disk mutation — callers delete layout and write after offline.
[[nodiscard]] base::Result<PreparedTargetPartitionTable>
prepare_target_partition_table_stage(const PrepareTargetPartitionTableArgs& args) {
    using Prepared = PreparedTargetPartitionTable;
    if (args.request == nullptr || args.source_disk == nullptr || args.targets == nullptr) {
        return base::Result<Prepared>::failure(
            {base::ErrorCode::kInvalidArgument, "partition table prepare arguments are incomplete"});
    }
    const auto& request = *args.request;
    const auto& source_disk = *args.source_disk;
    const auto& targets = *args.targets;
    ScopedStage stage(WorkerTaskLog::active(), "prepare_target_partition_table");
    const auto style = partition_style_name(source_disk.partition_style);
    stage.note("partition_style", style);
    stage.note("preserve_disk_signature", request.preserve_disk_signature ? "true" : "false");
    stage.note_u64("layout_edits", request.partition_layout_edits.size());
    auto layout = adapters::windows_disk::apply_disk_signature_policy(
        to_windows_raw_layout(source_disk.raw_layout), request.preserve_disk_signature, style);
    if (!layout) {
        stage.fail(layout.error(), "signature_policy", stage_hint(layout.error()));
        return base::Result<Prepared>::failure(layout.error());
    }
    const auto keep_offsets = keep_partition_offsets_for_disk_restore(source_disk, targets);
    stage.note_u64("keep_partition_count", keep_offsets.size());
    auto filtered = adapters::windows_disk::remove_unbacked_basic_data_partitions(
        std::move(layout).value(), style, source_disk.bytes_per_sector, keep_offsets);
    if (!filtered) {
        stage.fail(filtered.error(), "filter_unbacked_partitions", stage_hint(filtered.error()));
        return base::Result<Prepared>::failure(filtered.error());
    }
    adapters::windows_disk::PartitionLayoutRequest preflight{
        &filtered.value(), style, source_disk.bytes_per_sector, args.target_disk_size_bytes, {}};
    auto preflight_ok = adapters::windows_disk::validate_raw_disk_layout_for_restore(preflight);
    if (!preflight_ok) {
        stage.fail(preflight_ok.error(), "validate_raw_layout", stage_hint(preflight_ok.error()));
        return base::Result<Prepared>::failure(preflight_ok.error());
    }
    auto covers = ensure_layout_covers_volume_payloads(request.partition_layout_edits, targets,
                                                       source_disk.bytes_per_sector);
    if (!covers) {
        stage.fail(covers.error(), "layout_vs_payload", stage_hint(covers.error()));
        return base::Result<Prepared>::failure(covers.error());
    }
    auto adapter_edits =
        to_adapter_layout_edits(request.partition_layout_edits, source_disk.bytes_per_sector);
    if (!adapter_edits) {
        stage.fail(adapter_edits.error(), "adapter_layout_edits",
                   stage_hint(adapter_edits.error()));
        return base::Result<Prepared>::failure(adapter_edits.error());
    }
    adapters::windows_disk::PartitionLayoutRequest resolve_request{
        &filtered.value(), style, source_disk.bytes_per_sector, args.target_disk_size_bytes,
        adapter_edits.value()};
    auto resolved = adapters::windows_disk::resolve_partition_layout_edits(resolve_request);
    if (!resolved) {
        stage.fail(resolved.error(), "resolve_target_layout", stage_hint(resolved.error()));
        return base::Result<Prepared>::failure(resolved.error());
    }
    std::vector<contracts::RestorePartitionLayoutEdit> contracts_edits;
    contracts_edits.reserve(resolved.value().size());
    for (const auto& e : resolved.value()) {
        contracts_edits.push_back(
            {e.source_start_offset_bytes, e.target_start_offset_bytes, e.size_bytes});
    }
    auto resolved_covers =
        ensure_layout_covers_volume_payloads(contracts_edits, targets, source_disk.bytes_per_sector);
    if (!resolved_covers) {
        stage.fail(resolved_covers.error(), "resolved_layout_vs_payload",
                   stage_hint(resolved_covers.error()));
        return base::Result<Prepared>::failure(resolved_covers.error());
    }
    stage.note_u64("resolved_layout_edits", contracts_edits.size());
    adapters::windows_disk::PartitionLayoutRequest apply_request{
        nullptr, style, source_disk.bytes_per_sector, args.target_disk_size_bytes,
        resolved.value()};
    auto laid_out = adapters::windows_disk::apply_partition_layout_edits(
        std::move(filtered).value(), apply_request);
    if (!laid_out) {
        stage.fail(laid_out.error(), "apply_target_layout", stage_hint(laid_out.error()));
        return base::Result<Prepared>::failure(laid_out.error());
    }
    stage.note("partition_table", "prepared_in_memory");
    return base::Result<Prepared>::success(
        Prepared{std::move(contracts_edits), std::move(laid_out).value(), std::string(style)});
}

[[nodiscard]] base::Result<void>
finalize_target_disk_stage(
    const PersonalArchiveRestoreBackendRequest& request, const format::Disk& source_disk,
    const std::uint32_t target_disk,
    const std::vector<contracts::RestorePartitionLayoutEdit>& resolved_layout_edits) {
    ScopedStage stage(WorkerTaskLog::active(), "finalize_target_disk");
    const auto style = partition_style_name(source_disk.partition_style);
    if (!request.bring_target_online) {
        // Partition table / payload already written. FSCTL_EXTEND_VOLUME needs mounted
        // volumes, so skip filesystem fill when the caller leaves the disk offline.
        stage.note("bring_online", "skipped");
        if (!resolved_layout_edits.empty()) {
            stage.note("extend_filesystem", "skipped_offline");
        } else if (request.auto_expand_last_partition) {
            stage.note("auto_expand", "skipped_offline");
        }
        return base::Result<void>::success();
    }
    auto online = adapters::windows_disk::bring_target_disk_online(target_disk);
    if (!online) {
        stage.fail(online.error(), "bring_online", stage_hint(online.error()));
        return base::Result<void>::failure(online.error());
    }
    stage.note("bring_online", "ok");
    // FS fill at resolved partition starts (not raw UI hints); requires online volumes.
    if (!resolved_layout_edits.empty()) {
        for (const auto& edit : resolved_layout_edits) {
            auto extended = adapters::windows_disk::extend_filesystem_to_partition(
                target_disk, edit.target_start_offset_bytes);
            if (!extended) {
                stage.fail(extended.error(), "extend_filesystem", stage_hint(extended.error()));
                return base::Result<void>::failure(extended.error());
            }
        }
        stage.note("extend_filesystem", "ok");
    } else if (request.auto_expand_last_partition) {
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
        if (request.volume_size_policy == contracts::VolumeSizePolicy::kAllowNtfsRelocation &&
            !request.shrink_plan_digest.empty()) {
            return run_shrink_volume_restore(request, cancellation);
        }
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
            const auto write_limit = request.plan.logical_write_limit_bytes == 0
                                         ? volume_reader->logical_size_bytes()
                                         : request.plan.logical_write_limit_bytes;
            if (write_limit > sink->capacity_bytes()) {
                return fail_restore(stage,
                                    {base::ErrorCode::kInsufficientSpace,
                                     "restore target capacity is insufficient"},
                                    "capacity_preflight");
            }
            if (request.plan.logical_write_limit_bytes == 0) {
                stage.note("preflight", "source_size <= target_capacity");
            } else {
                stage.note_bytes("logical_write_limit", request.plan.logical_write_limit_bytes);
                stage.note("preflight", "prefix_restore_limit <= target_capacity");
            }
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
            stage.note_bytes("disk_written_bytes", summary.value().disk_written_bytes);
            stage.note_bytes("free_skipped_bytes", summary.value().free_skipped_bytes);
            stage.note_u64("free_ranges", summary.value().free_range_count);
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
        // Phase 1 (reversible): geometry + protected sources + in-memory final layout.
        {
            ScopedStage stage(WorkerTaskLog::active(), "validate_target_disk");
            stage.note("target", path_display(request.target));
            stage.note_u64("physical_drive", target_disk.value());
            adapters::windows_disk::TargetDiskGeometryCheck check{
                target_disk.value(), plan.value().source_disk->disk_size,
                plan.value().source_disk->bytes_per_sector};
            auto validated = adapters::windows_disk::validate_target_disk_for_raw_restore(check);
            if (!validated) {
                return fail_restore(stage, validated.error(), "geometry_or_system");
            }
        }
        const auto protected_for_sink = protected_sources;
        auto disk_sink =
            open_disk_sink_stage(request, *plan.value().source_disk, std::move(protected_sources));
        if (!disk_sink) {
            return base::Result<pipeline::RestoreSummary>::failure(disk_sink.error());
        }
        const auto target_size = disk_sink.value()->capacity_bytes();
        disk_sink.value().reset();
        auto table = prepare_target_partition_table_stage(
            PrepareTargetPartitionTableArgs{&request, plan.value().source_disk,
                                            &plan.value().targets, target_size});
        if (!table) {
            return base::Result<pipeline::RestoreSummary>::failure(table.error());
        }
        const auto& resolved_edits = table.value().resolved_edits;
        // Phase 2 (irreversible): offline → delete layout → write table → volume data → online.
        {
            ScopedStage stage(WorkerTaskLog::active(), "set_target_disk_offline");
            auto offline = adapters::windows_disk::set_target_disk_offline(target_disk.value());
            if (!offline) {
                return fail_restore(stage, offline.error(), "set_offline");
            }
            stage.note("offline", "ok");
        }
        {
            ScopedStage stage(WorkerTaskLog::active(), "delete_target_layout");
            auto deleted =
                adapters::windows_disk::delete_target_disk_drive_layout(target_disk.value());
            if (!deleted) {
                return fail_restore(stage, deleted.error(), "delete_layout");
            }
        }
        {
            ScopedStage stage(WorkerTaskLog::active(), "write_target_partition_table");
            adapters::windows_disk::RebuildPartitionTableRequest rebuild{
                target_disk.value(), plan.value().source_disk->bytes_per_sector,
                plan.value().source_disk->disk_size, &table.value().layout,
                table.value().partition_style};
            auto written = adapters::windows_disk::rebuild_partition_table_from_raw_layout(rebuild);
            if (!written) {
                return fail_restore(stage, written.error(), "write_mbr_gpt");
            }
            stage.note("partition_table", "written");
        }
        apply_layout_edits_to_volume_targets(plan.value().targets, resolved_edits,
                                             plan.value().source_disk->bytes_per_sector);
        disk_sink =
            open_disk_sink_stage(request, *plan.value().source_disk, protected_for_sink);
        if (!disk_sink) {
            return base::Result<pipeline::RestoreSummary>::failure(disk_sink.error());
        }
        auto summary = restore_disk_volumes_stage(RestoreDiskVolumesArgs{
            reader.value().get(), &reader.value()->manifest(), disk_sink.value().get(),
            &plan.value().targets, &request, &cancellation});
        if (!summary) {
            return summary;
        }
        disk_sink.value().reset();
        auto finalized = finalize_target_disk_stage(request, *plan.value().source_disk,
                                                    target_disk.value(), resolved_edits);
        if (!finalized) {
            return base::Result<pipeline::RestoreSummary>::failure(finalized.error());
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
