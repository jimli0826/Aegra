#include "aegra/adapters/personal_archive/personal_archive.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
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

/// One source volume's extent on the presented disk. volume_base is the volume offset of
/// the extent's first byte (0: single-extent volumes only, enforced by build_disk_regions).
struct DiskRegion final {
    std::uint64_t disk_offset{0};
    std::uint64_t length{0};
    std::uint32_t volume_index{0};
    std::uint64_t volume_base{0};
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
        });
    }
    std::sort(regions.begin(), regions.end(), [](const DiskRegion& left, const DiskRegion& right) {
        return left.disk_offset < right.disk_offset;
    });
    return base::Result<std::vector<DiskRegion>>::success(std::move(regions));
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
    explicit Impl(PersonalArchiveChainReader& chain_reader) : chain(&chain_reader) {}

    // Everything below is immutable after open(); the counters are the only mutable state.
    PersonalArchiveChainReader* chain{nullptr};
    format::Disk disk{};
    std::uint32_t disk_number{0};
    std::uint64_t disk_size{0};
    std::uint32_t sector_size{512};
    format::PartitionStyle partition_style{format::PartitionStyle::kRaw};
    format::RawDiskLayout raw_layout{};
    std::vector<DiskRegion> regions;

    // Default (sequentially consistent) ordering: diagnostics only, never control flow.
    std::atomic<std::uint64_t> read_calls{0};
    std::atomic<std::uint64_t> bytes_read{0};
};

WholeDiskByteReader::WholeDiskByteReader(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

WholeDiskByteReader::~WholeDiskByteReader() = default;

base::Result<std::unique_ptr<WholeDiskByteReader>>
WholeDiskByteReader::open(PersonalArchiveChainReader& chain, const format::Manifest& manifest,
                          const std::uint32_t source_disk_number,
                          const WholeDiskReaderOptions& options) {
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

    auto implementation = std::make_unique<Impl>(chain);
    implementation->disk = *disk;
    implementation->disk_number = source_disk_number;
    implementation->disk_size = disk->disk_size;
    implementation->sector_size = disk->bytes_per_sector > 0 ? disk->bytes_per_sector : 512U;
    implementation->partition_style = disk->partition_style;
    implementation->raw_layout = disk->raw_layout;
    if (options.disk_identity == WholeDiskIdentity::kAssignUnique) {
        randomize_disk_identity(implementation->raw_layout, implementation->partition_style);
    }
    implementation->regions = std::move(regions).value();
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
        const auto volume_offset = region.volume_base + (from - region.disk_offset);
        const auto slice = window.subspan(static_cast<std::size_t>(from - offset),
                                          static_cast<std::size_t>(to - from));
        auto filled = implementation_->chain->read_range(region.volume_index, volume_offset, slice,
                                                         cancellation);
        if (!filled) {
            return base::Result<std::size_t>::failure(filled.error());
        }
    }

    overlay_raw_layout(implementation_->raw_layout, implementation_->partition_style,
                       implementation_->sector_size, implementation_->disk_size, offset, window);
    ++implementation_->read_calls;
    implementation_->bytes_read += to_read;
    return base::Result<std::size_t>::success(to_read);
}

WholeDiskReadStatistics WholeDiskByteReader::statistics() const noexcept {
    WholeDiskReadStatistics result;
    result.read_calls = implementation_->read_calls.load();
    result.bytes_read = implementation_->bytes_read.load();
    return result;
}

} // namespace aegra::adapters::personal_archive
