#include "personal_archive_restore_task_backend.h"

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
        adapters::personal_archive::ArchiveChainOpenRequest open_request;
        open_request.maximum_chain_depth = request.maximum_chain_depth;
        std::vector<std::filesystem::path> protected_sources;
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
        auto reader = adapters::personal_archive::PersonalArchiveChainReader::open(open_request);
        if (!reader) {
            return base::Result<pipeline::RestoreSummary>::failure(reader.error());
        }
        auto sink = adapters::windows_disk::WindowsBlockSink::open(
            {request.target, adapters::windows_disk::WindowsBlockSinkKind::kVolume, std::nullopt,
             std::move(protected_sources)});
        if (!sink) {
            return base::Result<pipeline::RestoreSummary>::failure(sink.error());
        }
        pipeline::RestorePipeline restore(*reader.value(), *sink.value(), request.progress);
        return restore.run(request.plan, cancellation);
    }

    [[nodiscard]] static base::Result<pipeline::RestoreSummary>
    run_disk_restore(const PersonalArchiveRestoreBackendRequest& request,
                     const base::CancellationToken& cancellation) {
        if (request.layers.empty()) {
            return base::Result<pipeline::RestoreSummary>::failure(
                {base::ErrorCode::kInvalidArgument, "disk restore requires at least one archive"});
        }
        adapters::personal_archive::ArchiveChainOpenRequest open_request;
        open_request.maximum_chain_depth = request.maximum_chain_depth;
        std::vector<std::filesystem::path> protected_sources;
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
        // Chain reader validates Full base, parent UUID links, set, and volume geometry.
        auto reader = adapters::personal_archive::PersonalArchiveChainReader::open(open_request);
        if (!reader) {
            return base::Result<pipeline::RestoreSummary>::failure(reader.error());
        }
        // Tip manifest: same volume geometry as Full; raw_layout re-captured each backup.
        const auto& manifest = reader.value()->manifest();
        auto source_disk = find_source_disk(manifest, request.source_disk_number);
        if (!source_disk) {
            return base::Result<pipeline::RestoreSummary>::failure(source_disk.error());
        }
        if (source_disk.value()->disk_size == 0) {
            return base::Result<pipeline::RestoreSummary>::failure(
                {base::ErrorCode::kCorruptData, "source disk size is missing from manifest"});
        }
        const auto has_raw_layout = !source_disk.value()->raw_layout.mbr_sector.empty() ||
                                    source_disk.value()->partition_style == format::PartitionStyle::kRaw;
        if (!has_raw_layout) {
            return base::Result<pipeline::RestoreSummary>::failure(
                {base::ErrorCode::kCorruptData,
                 "archive lacks raw_layout; re-backup the disk before disk restore"});
        }
        auto targets = plan_disk_volumes(manifest, request.source_disk_number);
        if (!targets) {
            return base::Result<pipeline::RestoreSummary>::failure(targets.error());
        }
        auto target_disk = adapters::windows_disk::WindowsBlockSink::physical_drive_number(
            request.target);
        if (!target_disk) {
            return base::Result<pipeline::RestoreSummary>::failure(
                {base::ErrorCode::kInvalidArgument, "disk restore target must be PhysicalDrive"});
        }
        auto prepared = adapters::windows_disk::prepare_target_disk_for_raw_restore(
            target_disk.value(), source_disk.value()->disk_size,
            source_disk.value()->bytes_per_sector);
        if (!prepared) {
            return base::Result<pipeline::RestoreSummary>::failure(prepared.error());
        }
        adapters::windows_disk::WindowsBlockSinkOpenRequest sink_request;
        sink_request.path = request.target;
        sink_request.kind = adapters::windows_disk::WindowsBlockSinkKind::kPhysicalDisk;
        sink_request.protected_sources = std::move(protected_sources);
        sink_request.minimum_capacity_bytes = source_disk.value()->disk_size;
        sink_request.expected_bytes_per_sector = source_disk.value()->bytes_per_sector;
        auto opened_sink = adapters::windows_disk::WindowsBlockSink::open(sink_request);
        if (!opened_sink) {
            return base::Result<pipeline::RestoreSummary>::failure(opened_sink.error());
        }
        auto disk_sink = std::move(opened_sink).value();
        pipeline::RestoreSummary summary;
        for (const auto& target : targets.value()) {
            if (cancellation.stop_requested()) {
                return base::Result<pipeline::RestoreSummary>::failure(
                    {base::ErrorCode::kCancelled, "disk restore cancelled"});
            }
            auto volume_reader = adapters::personal_archive::PersonalArchiveVolumeReader::open(
                *reader.value(), manifest, target.volume_index);
            if (!volume_reader) {
                return base::Result<pipeline::RestoreSummary>::failure(volume_reader.error());
            }
            OffsetBlockSink offset_sink(*disk_sink, target.physical_offset, target.volume_size);
            pipeline::RestorePipeline restore(*volume_reader.value(), offset_sink, request.progress);
            auto volume_summary = restore.run(request.plan, cancellation);
            if (!volume_summary) {
                return base::Result<pipeline::RestoreSummary>::failure(volume_summary.error());
            }
            summary.restored_bytes += volume_summary.value().restored_bytes;
            summary.chunk_count += volume_summary.value().chunk_count;
            summary.peak_buffered_bytes =
                (std::max)(summary.peak_buffered_bytes,
                           volume_summary.value().peak_buffered_bytes);
        }
        auto flushed = disk_sink->flush(cancellation);
        if (!flushed) {
            return base::Result<pipeline::RestoreSummary>::failure(flushed.error());
        }
        // Close data handle before rewriting partition table.
        disk_sink.reset();
        const auto style = partition_style_name(source_disk.value()->partition_style);
        auto layout = adapters::windows_disk::apply_disk_signature_policy(
            to_windows_raw_layout(source_disk.value()->raw_layout),
            request.preserve_disk_signature, style);
        if (!layout) {
            return base::Result<pipeline::RestoreSummary>::failure(layout.error());
        }
        auto rebuilt = adapters::windows_disk::rebuild_partition_table_from_raw_layout(
            target_disk.value(), source_disk.value()->bytes_per_sector,
            source_disk.value()->disk_size, layout.value(), style);
        if (!rebuilt) {
            return base::Result<pipeline::RestoreSummary>::failure(rebuilt.error());
        }
        if (request.bring_target_online) {
            auto online = adapters::windows_disk::bring_target_disk_online(target_disk.value());
            if (!online) {
                return base::Result<pipeline::RestoreSummary>::failure(online.error());
            }
        }
        if (request.auto_expand_last_partition) {
            auto expanded = adapters::windows_disk::expand_last_data_partition_on_disk(
                target_disk.value(), source_disk.value()->disk_size,
                source_disk.value()->bytes_per_sector, style);
            if (!expanded) {
                return base::Result<pipeline::RestoreSummary>::failure(expanded.error());
            }
        }
        return base::Result<pipeline::RestoreSummary>::success(summary);
    }
};

} // namespace

std::unique_ptr<IPersonalArchiveRestoreTaskBackend>
make_personal_archive_restore_task_backend() {
    return std::make_unique<PersonalArchiveRestoreTaskBackend>();
}

} // namespace aegra::apps::worker::detail
