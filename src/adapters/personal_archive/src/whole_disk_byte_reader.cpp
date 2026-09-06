#include "aegra/adapters/personal_archive/personal_archive.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

[[nodiscard]] base::Error make_error(const base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] std::uint32_t crc32_ieee(const std::span<const std::byte> data) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : data) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<unsigned char>(byte));
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1U)));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

void write_le32(std::vector<std::byte>& buffer, const std::size_t offset,
                const std::uint32_t value) noexcept {
    if (offset + 4 > buffer.size()) {
        return;
    }
    buffer[offset] = static_cast<std::byte>(value & 0xFFU);
    buffer[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    buffer[offset + 2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    buffer[offset + 3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

[[nodiscard]] std::uint32_t read_le32(const std::byte* data) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

// Give the presented disk a fresh MBR signature and GPT DiskGUID so it cannot
// collide with a still-online source disk on the host. Edits the caller's
// in-memory raw-layout copy only; the archive on disk is never modified.
void randomize_disk_identity(format::RawDiskLayout& layout,
                             const format::PartitionStyle partition_style) {
    std::random_device device;

    // MBR disk signature at offset 0x1B8 (present in a real MBR and in the
    // protective MBR of a GPT disk).
    if (layout.mbr_sector.size() >= 512U) {
        write_le32(layout.mbr_sector, 0x1B8U, static_cast<std::uint32_t>(device()));
    }

    if (partition_style != format::PartitionStyle::kGpt) {
        return;
    }

    // 16-byte GPT DiskGUID; primary and backup headers must carry the same one.
    std::array<std::byte, 16> disk_guid{};
    for (auto& item : disk_guid) {
        item = static_cast<std::byte>(device() & 0xFFU);
    }

    // GPT header layout (little-endian): 12 HeaderSize, 16 HeaderCRC32,
    // 56 DiskGUID (16 bytes), 88 PartitionEntryArrayCRC32. Only the DiskGUID
    // changes, so the entry-array CRC stays valid; recompute the header CRC over
    // HeaderSize bytes with the CRC field zeroed.
    const auto patch_gpt_header = [&disk_guid](std::vector<std::byte>& header) {
        constexpr std::size_t kMinHeaderSize = 92;
        constexpr std::size_t kDiskGuidOffset = 56;
        constexpr std::size_t kHeaderCrcOffset = 16;
        if (header.size() < kMinHeaderSize) {
            return;
        }
        std::memcpy(header.data() + kDiskGuidOffset, disk_guid.data(), disk_guid.size());
        write_le32(header, kHeaderCrcOffset, 0);
        std::uint32_t header_size = read_le32(header.data() + 12);
        if (header_size < kMinHeaderSize || header_size > header.size()) {
            header_size = kMinHeaderSize;
        }
        write_le32(header, kHeaderCrcOffset,
                   crc32_ieee(std::span<const std::byte>(header.data(), header_size)));
    };
    patch_gpt_header(layout.gpt_primary_header);
    patch_gpt_header(layout.gpt_backup_header);
}

[[nodiscard]] const format::Disk* find_disk(const format::Manifest& manifest,
                                            const std::uint32_t disk_number) noexcept {
    for (const auto& disk : manifest.disks) {
        if (disk.disk_number == disk_number) {
            return &disk;
        }
    }
    return nullptr;
}

struct DiskRegion final {
    std::uint64_t disk_offset{0};
    std::uint64_t length{0};
    std::uint32_t volume_index{0};
    std::uint64_t volume_base{0};
    std::uint64_t volume_size{0};
};

struct VolumeChunkSpan final {
    std::uint64_t logical_offset{0};
    std::uint64_t logical_size{0};
    std::uint64_t inner_chunk_index{0};
};

struct VolumeIndex final {
    std::uint32_t volume_index{0};
    std::uint64_t volume_size{0};
    std::vector<VolumeChunkSpan> chunks;
};

using ChunkPayload = std::shared_ptr<const std::vector<std::byte>>;

/// Bounded LRU of decompressed chunks. Interleaved random reads (a booting or
/// mounted guest) alternate between chunks, so a single-entry cache thrashes
/// into a full read+authenticate+decompress per request. Entries are shared so
/// a reader thread keeps a payload alive even after a concurrent miss evicts it.
class ChunkCache final {
  public:
    explicit ChunkCache(const std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    [[nodiscard]] ChunkPayload find(const std::uint64_t chunk_index) {
        std::lock_guard lock(mutex_);
        for (auto& entry : entries_) {
            if (entry.chunk_index == chunk_index) {
                entry.last_used = ++use_counter_;
                return entry.payload;
            }
        }
        return nullptr;
    }

    void insert(const std::uint64_t chunk_index, ChunkPayload payload) {
        std::lock_guard lock(mutex_);
        for (auto& entry : entries_) {
            if (entry.chunk_index == chunk_index) {
                entry.payload = std::move(payload);
                entry.last_used = ++use_counter_;
                return;
            }
        }
        if (entries_.size() < capacity_) {
            entries_.push_back(Entry{chunk_index, std::move(payload), ++use_counter_});
            return;
        }
        auto oldest = std::min_element(
            entries_.begin(), entries_.end(),
            [](const Entry& left, const Entry& right) { return left.last_used < right.last_used; });
        *oldest = Entry{chunk_index, std::move(payload), ++use_counter_};
    }

  private:
    struct Entry final {
        std::uint64_t chunk_index{0};
        ChunkPayload payload;
        std::uint64_t last_used{0};
    };

    std::mutex mutex_;
    std::size_t capacity_{1};
    std::uint64_t use_counter_{0};
    std::vector<Entry> entries_;
};

/// Chunk provider handed through the read path: authoritative reader + cache.
struct ChunkSource final {
    ports::IRecoveryPointReader& inner;
    ChunkCache& cache;
};

void overlay_region(const std::uint64_t request_offset, const std::uint64_t request_end,
                    const std::span<std::byte> buffer, const std::uint64_t region_offset,
                    const std::vector<std::byte>& bytes) noexcept {
    if (bytes.empty()) {
        return;
    }
    const auto region_end = region_offset + bytes.size();
    const auto from = (std::max)(request_offset, region_offset);
    const auto to = (std::min)(request_end, region_end);
    if (from >= to) {
        return;
    }
    std::memcpy(buffer.data() + (from - request_offset), bytes.data() + (from - region_offset),
                static_cast<std::size_t>(to - from));
}

[[nodiscard]] base::Result<std::vector<VolumeIndex>>
build_volume_indices(ports::IRecoveryPointReader& inner, const format::Manifest& manifest) {
    std::vector<VolumeIndex> indices;
    indices.reserve(manifest.volumes.size());
    for (const auto& volume : manifest.volumes) {
        VolumeIndex index;
        index.volume_index = volume.volume_index;
        index.volume_size = volume.total_size;
        indices.push_back(std::move(index));
    }

    for (std::uint64_t chunk_index = 0; chunk_index < inner.chunk_count(); ++chunk_index) {
        auto descriptor = inner.describe_chunk(chunk_index);
        if (!descriptor) {
            return base::Result<std::vector<VolumeIndex>>::failure(descriptor.error());
        }
        const auto source = descriptor.value().source_index;
        auto volume =
            std::find_if(indices.begin(), indices.end(), [source](const VolumeIndex& candidate) {
                return candidate.volume_index == source;
            });
        if (volume == indices.end()) {
            continue;
        }
        if (descriptor.value().logical_size == 0) {
            return base::Result<std::vector<VolumeIndex>>::failure(
                make_error(base::ErrorCode::kCorruptData, "chunk logical size is zero"));
        }
        volume->chunks.push_back(VolumeChunkSpan{
            .logical_offset = descriptor.value().logical_offset,
            .logical_size = descriptor.value().logical_size,
            .inner_chunk_index = chunk_index,
        });
    }

    for (auto& volume : indices) {
        std::sort(volume.chunks.begin(), volume.chunks.end(),
                  [](const VolumeChunkSpan& left, const VolumeChunkSpan& right) {
                      return left.logical_offset < right.logical_offset;
                  });
    }
    return base::Result<std::vector<VolumeIndex>>::success(std::move(indices));
}

[[nodiscard]] base::Result<std::vector<DiskRegion>>
build_disk_regions(const format::Manifest& manifest, const std::uint32_t disk_number) {
    std::vector<DiskRegion> regions;
    for (const auto& volume : manifest.volumes) {
        std::optional<std::uint64_t> physical_offset;
        std::uint64_t covered = 0;
        for (const auto& extent : volume.extents) {
            if (extent.disk_number != disk_number || extent.length == 0) {
                continue;
            }
            if (physical_offset.has_value() &&
                physical_offset.value() + covered != extent.physical_offset) {
                return base::Result<std::vector<DiskRegion>>::failure(make_error(
                    base::ErrorCode::kUnsupportedVersion,
                    "multi-extent volumes on the source disk are not supported for mount"));
            }
            if (!physical_offset.has_value()) {
                physical_offset = extent.physical_offset;
            }
            if (extent.volume_offset != covered) {
                return base::Result<std::vector<DiskRegion>>::failure(
                    make_error(base::ErrorCode::kCorruptData,
                               "volume extent volume_offset is not contiguous"));
            }
            covered += extent.length;
        }
        if (!physical_offset.has_value() || volume.total_size == 0) {
            continue;
        }
        if (covered < volume.total_size) {
            return base::Result<std::vector<DiskRegion>>::failure(
                make_error(base::ErrorCode::kCorruptData,
                           "volume extents do not cover volume size on source disk"));
        }
        regions.push_back(DiskRegion{
            .disk_offset = physical_offset.value(),
            .length = volume.total_size,
            .volume_index = volume.volume_index,
            .volume_base = 0,
            .volume_size = volume.total_size,
        });
    }
    std::sort(regions.begin(), regions.end(), [](const DiskRegion& left, const DiskRegion& right) {
        return left.disk_offset < right.disk_offset;
    });
    return base::Result<std::vector<DiskRegion>>::success(std::move(regions));
}

[[nodiscard]] const VolumeIndex* find_volume(const std::vector<VolumeIndex>& volumes,
                                             const std::uint32_t volume_index) noexcept {
    for (const auto& volume : volumes) {
        if (volume.volume_index == volume_index) {
            return &volume;
        }
    }
    return nullptr;
}

[[nodiscard]] base::Result<ChunkPayload> load_chunk(const ChunkSource& source,
                                                    const std::uint64_t inner_chunk_index,
                                                    const base::CancellationToken cancellation) {
    if (auto cached = source.cache.find(inner_chunk_index)) {
        return base::Result<ChunkPayload>::success(std::move(cached));
    }
    auto chunk = source.inner.read_chunk(inner_chunk_index, cancellation);
    if (!chunk) {
        return base::Result<ChunkPayload>::failure(chunk.error());
    }
    auto payload = std::make_shared<const std::vector<std::byte>>(std::move(chunk).value().payload);
    source.cache.insert(inner_chunk_index, payload);
    return base::Result<ChunkPayload>::success(std::move(payload));
}

[[nodiscard]] base::Result<void> read_volume_range(const ChunkSource& source,
                                                   const VolumeIndex& volume,
                                                   const std::uint64_t volume_offset,
                                                   const std::span<std::byte> destination,
                                                   const base::CancellationToken cancellation) {
    if (destination.empty() || volume_offset >= volume.volume_size) {
        return base::Result<void>::success();
    }
    auto remaining = destination;
    auto cursor = volume_offset;
    while (!remaining.empty() && cursor < volume.volume_size) {
        const auto chunk =
            std::lower_bound(volume.chunks.begin(), volume.chunks.end(), cursor,
                             [](const VolumeChunkSpan& span, const std::uint64_t offset) {
                                 return span.logical_offset + span.logical_size <= offset;
                             });
        if (chunk == volume.chunks.end() || cursor < chunk->logical_offset) {
            const auto gap_end =
                chunk == volume.chunks.end() ? volume.volume_size : chunk->logical_offset;
            const auto skip =
                (std::min)(static_cast<std::uint64_t>(remaining.size()), gap_end - cursor);
            remaining = remaining.subspan(static_cast<std::size_t>(skip));
            cursor += skip;
            continue;
        }
        const auto into_chunk = cursor - chunk->logical_offset;
        auto payload = load_chunk(source, chunk->inner_chunk_index, cancellation);
        if (!payload) {
            return base::Result<void>::failure(payload.error());
        }
        if (payload.value()->size() < chunk->logical_size) {
            return base::Result<void>::failure(make_error(
                base::ErrorCode::kCorruptData, "chunk payload is shorter than logical size"));
        }
        const auto copy_size = (std::min)(static_cast<std::uint64_t>(remaining.size()),
                                          chunk->logical_size - into_chunk);
        std::memcpy(remaining.data(), payload.value()->data() + into_chunk,
                    static_cast<std::size_t>(copy_size));
        remaining = remaining.subspan(static_cast<std::size_t>(copy_size));
        cursor += copy_size;
    }
    return base::Result<void>::success();
}

void overlay_raw_layout(const format::RawDiskLayout& raw_layout,
                        const format::PartitionStyle partition_style,
                        const std::uint32_t sector_size, const std::uint64_t disk_size,
                        const std::uint64_t offset, const std::span<std::byte> buffer) noexcept {
    const auto request_end = offset + buffer.size();
    const auto sector = static_cast<std::uint64_t>(sector_size);
    overlay_region(offset, request_end, buffer, 0, raw_layout.mbr_sector);
    if (partition_style != format::PartitionStyle::kGpt) {
        return;
    }
    overlay_region(offset, request_end, buffer, sector, raw_layout.gpt_primary_header);
    overlay_region(offset, request_end, buffer, 2 * sector, raw_layout.gpt_partition_entries);
    if (disk_size < sector) {
        return;
    }
    const auto backup_header_offset = disk_size - sector;
    overlay_region(offset, request_end, buffer, backup_header_offset, raw_layout.gpt_backup_header);
    if (raw_layout.gpt_backup_entries.empty()) {
        return;
    }
    auto backup_entries_offset = backup_header_offset - raw_layout.gpt_backup_entries.size();
    backup_entries_offset = (backup_entries_offset / sector) * sector;
    overlay_region(offset, request_end, buffer, backup_entries_offset,
                   raw_layout.gpt_backup_entries);
}

} // namespace

struct WholeDiskByteReader::Impl final {
    explicit Impl(const std::size_t cache_chunk_count) : cache(cache_chunk_count) {}

    ports::IRecoveryPointReader* inner{nullptr};
    format::Disk disk{};
    std::uint32_t disk_number{0};
    std::uint64_t disk_size{0};
    std::uint32_t sector_size{512};
    format::PartitionStyle partition_style{format::PartitionStyle::kRaw};
    format::RawDiskLayout raw_layout{};
    std::vector<DiskRegion> regions;
    std::vector<VolumeIndex> volumes;
    mutable ChunkCache cache;
};

WholeDiskByteReader::WholeDiskByteReader(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

WholeDiskByteReader::~WholeDiskByteReader() = default;

base::Result<std::unique_ptr<WholeDiskByteReader>>
WholeDiskByteReader::open(ports::IRecoveryPointReader& inner, const format::Manifest& manifest,
                          const std::uint32_t source_disk_number,
                          const std::size_t cache_chunk_count,
                          const bool assign_unique_disk_identity) {
    const auto* disk = find_disk(manifest, source_disk_number);
    if (disk == nullptr) {
        return base::Result<std::unique_ptr<WholeDiskByteReader>>::failure(make_error(
            base::ErrorCode::kNotFound, "source disk is not present in archive manifest"));
    }
    if (disk->disk_size == 0) {
        return base::Result<std::unique_ptr<WholeDiskByteReader>>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "source disk size is zero"));
    }
    auto regions = build_disk_regions(manifest, source_disk_number);
    if (!regions) {
        return base::Result<std::unique_ptr<WholeDiskByteReader>>::failure(regions.error());
    }
    auto volumes = build_volume_indices(inner, manifest);
    if (!volumes) {
        return base::Result<std::unique_ptr<WholeDiskByteReader>>::failure(volumes.error());
    }

    auto implementation = std::make_unique<Impl>(cache_chunk_count);
    implementation->inner = &inner;
    implementation->disk = *disk;
    implementation->disk_number = source_disk_number;
    implementation->disk_size = disk->disk_size;
    implementation->sector_size = disk->bytes_per_sector > 0 ? disk->bytes_per_sector : 512U;
    implementation->partition_style = disk->partition_style;
    implementation->raw_layout = disk->raw_layout;
    if (assign_unique_disk_identity) {
        randomize_disk_identity(implementation->raw_layout, implementation->partition_style);
    }
    implementation->regions = std::move(regions).value();
    implementation->volumes = std::move(volumes).value();
    return base::Result<std::unique_ptr<WholeDiskByteReader>>::success(
        std::unique_ptr<WholeDiskByteReader>(new WholeDiskByteReader(std::move(implementation))));
}

std::uint64_t WholeDiskByteReader::size_bytes() const noexcept {
    return implementation_->disk_size;
}

std::uint32_t WholeDiskByteReader::source_disk_number() const noexcept {
    return implementation_->disk_number;
}

const format::Disk& WholeDiskByteReader::disk() const noexcept { return implementation_->disk; }

base::Result<std::size_t> WholeDiskByteReader::read_at(const std::uint64_t offset,
                                                       const std::span<std::byte> destination,
                                                       const base::CancellationToken cancellation) {
    if (destination.empty()) {
        return base::Result<std::size_t>::success(0);
    }
    if (offset >= implementation_->disk_size) {
        return base::Result<std::size_t>::success(0);
    }
    const auto max_readable = implementation_->disk_size - offset;
    const auto to_read = static_cast<std::size_t>(
        (std::min)(max_readable, static_cast<std::uint64_t>(destination.size())));
    const auto window = destination.subspan(0, to_read);
    std::memset(window.data(), 0, window.size());

    const auto request_end = offset + to_read;
    for (const auto& region : implementation_->regions) {
        const auto region_end = region.disk_offset + region.length;
        if (region_end <= offset || region.disk_offset >= request_end) {
            continue;
        }
        const auto from = (std::max)(offset, region.disk_offset);
        const auto to = (std::min)(request_end, region_end);
        if (from >= to) {
            continue;
        }
        const auto* volume = find_volume(implementation_->volumes, region.volume_index);
        if (volume == nullptr) {
            return base::Result<std::size_t>::failure(
                make_error(base::ErrorCode::kNotFound, "volume index is missing from chunk map"));
        }
        const auto volume_offset = region.volume_base + (from - region.disk_offset);
        const auto slice = window.subspan(static_cast<std::size_t>(from - offset),
                                          static_cast<std::size_t>(to - from));
        const ChunkSource chunk_source{*implementation_->inner, implementation_->cache};
        auto filled = read_volume_range(chunk_source, *volume, volume_offset, slice, cancellation);
        if (!filled) {
            return base::Result<std::size_t>::failure(filled.error());
        }
    }

    overlay_raw_layout(implementation_->raw_layout, implementation_->partition_style,
                       implementation_->sector_size, implementation_->disk_size, offset, window);
    return base::Result<std::size_t>::success(to_read);
}

} // namespace aegra::adapters::personal_archive
