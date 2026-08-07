#include "vhdx_disk_image.h"

#include "cow_backing_store.h"
#include "crc32c.h"

#include <rpc.h>

#include <cstring>

namespace aegra::adapters::dokan::detail {
namespace {

constexpr std::uint32_t kVhdxLogicalSectorSize = 512;
constexpr std::uint32_t kVhdxPhysicalSectorSize = 512;
constexpr std::uint32_t kVhdxBlockSize = 32 * 1024 * 1024;

constexpr std::uint64_t kVhdxHeaderSize = 1024 * 1024;
constexpr std::uint64_t kVhdxLogSize = 1024 * 1024;
constexpr std::uint64_t kVhdxRegionSize = 1024 * 1024;

constexpr std::uint64_t kBatStateFullyPresent = 6;

#pragma pack(push, 1)

struct VhdxFileIdentifier {
    std::uint8_t signature[8];
};

struct VhdxHeader {
    std::uint32_t signature;
    std::uint32_t checksum;
    std::uint64_t sequence_number;
    std::uint8_t file_write_guid[16];
    std::uint8_t data_write_guid[16];
    std::uint8_t log_guid[16];
    std::uint16_t log_version;
    std::uint16_t version;
    std::uint32_t log_length;
    std::uint64_t log_offset;
    std::uint8_t reserved[4016];
};
static_assert(sizeof(VhdxHeader) == 4096);

struct VhdxRegionTableHeader {
    std::uint32_t signature;
    std::uint32_t checksum;
    std::uint32_t entry_count;
    std::uint32_t reserved;
};

struct VhdxRegionTableEntry {
    std::uint8_t guid[16];
    std::uint64_t file_offset;
    std::uint32_t length;
    std::uint32_t required;
};

struct VhdxMetadataHeader {
    std::uint64_t signature;
    std::uint16_t reserved1;
    std::uint16_t entry_count;
    std::uint8_t reserved2[20];
};

struct VhdxMetadataEntry {
    std::uint8_t item_id[16];
    std::uint32_t offset;
    std::uint32_t length;
    std::uint32_t flags;
    std::uint32_t reserved;
};

struct VhdxFileParameters {
    std::uint32_t block_size;
    std::uint32_t flags;
};

struct VhdxVirtualDiskSize {
    std::uint64_t virtual_disk_size;
};

#pragma pack(pop)

const std::uint8_t kRegionGuidBat[16] = {0x66, 0x77, 0xC2, 0x2D, 0x23, 0xF6,
                                         0x00, 0x42, 0x9D, 0x64, 0x11, 0x5E,
                                         0x9B, 0xFD, 0x4A, 0x08};
const std::uint8_t kRegionGuidMetadata[16] = {
    0x06, 0xA2, 0x7C, 0x8B, 0x90, 0x47, 0x9A, 0x4B,
    0xB8, 0xFE, 0x57, 0x5F, 0x05, 0x0F, 0x88, 0x6E};
const std::uint8_t kMetaFileParameters[16] = {
    0x37, 0x67, 0xA1, 0xCA, 0x36, 0xFA, 0x43, 0x4D,
    0xB3, 0xB6, 0x33, 0xF0, 0xAA, 0x44, 0xE7, 0x6B};
const std::uint8_t kMetaVirtualDiskSize[16] = {
    0x24, 0x42, 0xA5, 0x2F, 0x1B, 0xCD, 0x76, 0x48,
    0xB2, 0x11, 0x5D, 0xBE, 0xD8, 0x3B, 0xF4, 0xB8};
const std::uint8_t kMetaDiskId[16] = {0xAB, 0x12, 0xCA, 0xBE, 0xE6, 0xB2,
                                      0x23, 0x45, 0x93, 0xEF, 0xC3, 0x09,
                                      0xE0, 0x00, 0xC7, 0x46};
const std::uint8_t kMetaLogicalSector[16] = {
    0x1D, 0xBF, 0x41, 0x81, 0x6F, 0xA9, 0x09, 0x47,
    0xBA, 0x47, 0xF2, 0x33, 0xA8, 0xFA, 0xAB, 0x5F};
const std::uint8_t kMetaPhysicalSector[16] = {
    0xC7, 0x48, 0xA3, 0xCD, 0x5D, 0x44, 0x71, 0x44,
    0x9C, 0xC9, 0xE9, 0x88, 0x52, 0x51, 0xC5, 0x56};

void generate_guid(std::uint8_t guid[16]) {
    UUID uuid{};
    if (UuidCreate(&uuid) == RPC_S_OK) {
        std::memcpy(guid, &uuid, 16);
        return;
    }

    union {
        FILETIME ft;
        std::uint64_t t;
    } u{};
    GetSystemTimeAsFileTime(&u.ft);
    const std::uint64_t t = u.t;
    const std::uint64_t pid = GetCurrentProcessId();
    const std::uint64_t tid = GetCurrentThreadId();
    for (int i = 0; i < 8; ++i) {
        guid[i] = static_cast<std::uint8_t>(t >> (i * 8));
        guid[8 + i] = static_cast<std::uint8_t>((pid ^ tid) >> (i * 8));
    }
    guid[6] = static_cast<std::uint8_t>((guid[6] & 0x0F) | 0x40);
    guid[8] = static_cast<std::uint8_t>((guid[8] & 0x3F) | 0x80);
}

void build_header(VhdxHeader* hdr, std::uint64_t seq, std::uint64_t log_offset,
                  const std::uint8_t* file_write_guid,
                  const std::uint8_t* data_write_guid) {
    ZeroMemory(hdr, sizeof(VhdxHeader));
    hdr->signature = 0x64616568; // "head"
    hdr->sequence_number = seq;
    std::memcpy(hdr->file_write_guid, file_write_guid, 16);
    std::memcpy(hdr->data_write_guid, data_write_guid, 16);
    hdr->log_version = 0;
    hdr->version = 1;
    hdr->log_length = static_cast<std::uint32_t>(kVhdxLogSize);
    hdr->log_offset = log_offset;
    hdr->checksum = Crc32c::compute(hdr, sizeof(VhdxHeader));
}

} // namespace

void VhdxDiskImage::compute_layout() {
    Layout& L = layout_;
    L.raw_data_size = static_cast<std::uint64_t>(backing_.size());

    L.virtual_disk_size =
        ((L.raw_data_size + kVhdxLogicalSectorSize - 1) / kVhdxLogicalSectorSize) *
        kVhdxLogicalSectorSize;

    L.block_size = kVhdxBlockSize;
    L.block_count = (L.virtual_disk_size + L.block_size - 1) / L.block_size;

    if (L.virtual_disk_size % L.block_size == 0 && L.virtual_disk_size > 0) {
        L.last_block_size = L.block_size;
    } else {
        L.last_block_size = L.virtual_disk_size % L.block_size;
    }

    L.chunk_ratio = (1ULL << 23) * kVhdxLogicalSectorSize / L.block_size;
    L.sector_bitmap_block_count =
        (L.block_count + L.chunk_ratio - 1) / L.chunk_ratio;

    L.bat_entry_count = L.block_count + L.sector_bitmap_block_count;
    L.bat_size = L.bat_entry_count * 8;
    L.bat_region_size =
        ((L.bat_size + kVhdxRegionSize - 1) / kVhdxRegionSize) * kVhdxRegionSize;
    L.metadata_region_size = kVhdxRegionSize;

    L.header_offset = 0;
    L.log_offset = kVhdxHeaderSize;
    L.metadata_offset = L.log_offset + kVhdxLogSize;
    L.bat_offset = L.metadata_offset + L.metadata_region_size;
    L.data_offset = L.bat_offset + L.bat_region_size;

    L.vhdx_file_size = L.data_offset + L.block_count * L.block_size;
}

void VhdxDiskImage::build_header_region() {
    const Layout& L = layout_;
    header_region_.assign(static_cast<std::size_t>(kVhdxHeaderSize), 0);

    auto* ident = reinterpret_cast<VhdxFileIdentifier*>(header_region_.data());
    std::memcpy(ident->signature, "vhdxfile", 8);

    std::uint8_t file_write_guid[16];
    std::uint8_t data_write_guid[16];
    generate_guid(file_write_guid);
    generate_guid(data_write_guid);

    auto* hdr1 = reinterpret_cast<VhdxHeader*>(header_region_.data() + 0x10000);
    build_header(hdr1, 1, L.log_offset, file_write_guid, data_write_guid);
    auto* hdr2 = reinterpret_cast<VhdxHeader*>(header_region_.data() + 0x20000);
    build_header(hdr2, 2, L.log_offset, file_write_guid, data_write_guid);

    for (const std::uint32_t off : {0x30000u, 0x40000u}) {
        std::uint8_t* table = header_region_.data() + off;
        ZeroMemory(table, 64 * 1024);

        auto* reg_hdr = reinterpret_cast<VhdxRegionTableHeader*>(table);
        reg_hdr->signature = 0x69676572; // "regi"
        reg_hdr->entry_count = 2;

        auto* reg_entry = reinterpret_cast<VhdxRegionTableEntry*>(
            table + sizeof(VhdxRegionTableHeader));

        std::memcpy(reg_entry[0].guid, kRegionGuidBat, 16);
        reg_entry[0].file_offset = L.bat_offset;
        reg_entry[0].length = static_cast<std::uint32_t>(L.bat_region_size);
        reg_entry[0].required = 1;

        std::memcpy(reg_entry[1].guid, kRegionGuidMetadata, 16);
        reg_entry[1].file_offset = L.metadata_offset;
        reg_entry[1].length = static_cast<std::uint32_t>(L.metadata_region_size);
        reg_entry[1].required = 1;

        reg_hdr->checksum = Crc32c::compute(table, 64 * 1024);
    }
}

void VhdxDiskImage::build_log_region() {
    log_region_.assign(static_cast<std::size_t>(kVhdxLogSize), 0);
}

void VhdxDiskImage::build_bat_data() {
    const Layout& L = layout_;

    bat_region_.assign(static_cast<std::size_t>(L.bat_region_size), 0);
    auto* bat_entries = reinterpret_cast<std::uint64_t*>(bat_region_.data());
    for (std::uint64_t i = 0; i < L.block_count; ++i) {
        const std::uint64_t bat_index = i + (i / L.chunk_ratio);
        const std::uint64_t block_offset = L.data_offset + i * L.block_size;
        bat_entries[bat_index] = block_offset | (kBatStateFullyPresent & 0x7);
    }

    metadata_region_.assign(static_cast<std::size_t>(L.metadata_region_size), 0);
    auto* meta_hdr = reinterpret_cast<VhdxMetadataHeader*>(metadata_region_.data());
    ZeroMemory(meta_hdr, sizeof(VhdxMetadataHeader));
    meta_hdr->signature = 0x617461646174656DULL; // "metadata"
    meta_hdr->entry_count = 5;

    auto* entries = reinterpret_cast<VhdxMetadataEntry*>(
        reinterpret_cast<std::uint8_t*>(meta_hdr) + sizeof(VhdxMetadataHeader));

    constexpr std::uint32_t kItemsBase = 64 * 1024;
    const std::uint32_t off_file_params = kItemsBase + 0;
    const std::uint32_t off_virtual_disk_size = kItemsBase + 8;
    const std::uint32_t off_logical_sector = kItemsBase + 16;
    const std::uint32_t off_physical_sector = kItemsBase + 20;
    const std::uint32_t off_virtual_disk_id = kItemsBase + 24;

    std::memcpy(entries[0].item_id, kMetaFileParameters, 16);
    entries[0].offset = off_file_params;
    entries[0].length = 8;
    entries[0].flags = 0x04;
    {
        auto* fp = reinterpret_cast<VhdxFileParameters*>(metadata_region_.data() +
                                                         off_file_params);
        fp->block_size = static_cast<std::uint32_t>(L.block_size);
        fp->flags = 0;
    }

    std::memcpy(entries[1].item_id, kMetaVirtualDiskSize, 16);
    entries[1].offset = off_virtual_disk_size;
    entries[1].length = 8;
    entries[1].flags = 0x06;
    {
        auto* vds = reinterpret_cast<VhdxVirtualDiskSize*>(
            metadata_region_.data() + off_virtual_disk_size);
        vds->virtual_disk_size = L.virtual_disk_size;
    }

    std::memcpy(entries[2].item_id, kMetaLogicalSector, 16);
    entries[2].offset = off_logical_sector;
    entries[2].length = 4;
    entries[2].flags = 0x06;
    *reinterpret_cast<std::uint32_t*>(metadata_region_.data() + off_logical_sector) =
        kVhdxLogicalSectorSize;

    std::memcpy(entries[3].item_id, kMetaPhysicalSector, 16);
    entries[3].offset = off_physical_sector;
    entries[3].length = 4;
    entries[3].flags = 0x06;
    *reinterpret_cast<std::uint32_t*>(metadata_region_.data() +
                                     off_physical_sector) = kVhdxPhysicalSectorSize;

    std::memcpy(entries[4].item_id, kMetaDiskId, 16);
    entries[4].offset = off_virtual_disk_id;
    entries[4].length = 16;
    entries[4].flags = 0x06;
    generate_guid(metadata_region_.data() + off_virtual_disk_id);
}

void VhdxDiskImage::rebuild_locked() {
    compute_layout();
    build_header_region();
    build_log_region();
    build_bat_data();
}

VhdxDiskImage::Region VhdxDiskImage::classify_offset(std::uint64_t vhdx_offset) const {
    const Layout& L = layout_;
    if (vhdx_offset < L.log_offset) {
        return Region::kHeader;
    }
    if (vhdx_offset < L.metadata_offset) {
        return Region::kLog;
    }
    if (vhdx_offset < L.bat_offset) {
        return Region::kMetadata;
    }
    if (vhdx_offset < L.data_offset) {
        return Region::kBat;
    }
    if (vhdx_offset < L.vhdx_file_size) {
        return Region::kData;
    }
    return Region::kOutOfRange;
}

bool VhdxDiskImage::to_raw_offset(std::uint64_t vhdx_offset, std::uint64_t* raw_offset,
                                  std::uint64_t* remaining) const {
    const Layout& L = layout_;
    if (vhdx_offset < L.data_offset) {
        return false;
    }
    const std::uint64_t data_offset = vhdx_offset - L.data_offset;
    *raw_offset = data_offset;
    *remaining = L.raw_data_size > data_offset ? L.raw_data_size - data_offset : 0;
    return true;
}

NTSTATUS VhdxDiskImage::read_locked(std::uint64_t vhdx_offset, void* buffer,
                                    DWORD buffer_len, LPDWORD bytes_read) {
    const Layout& L = layout_;
    *bytes_read = 0;

    if (vhdx_offset >= L.vhdx_file_size) {
        return STATUS_END_OF_FILE;
    }

    std::uint64_t available = L.vhdx_file_size - vhdx_offset;
    if (buffer_len > available) {
        buffer_len = static_cast<DWORD>(available);
    }

    auto* dst = static_cast<std::uint8_t*>(buffer);
    DWORD total_read = 0;

    while (total_read < buffer_len) {
        const std::uint64_t cur_offset = vhdx_offset + total_read;
        const Region region = classify_offset(cur_offset);
        DWORD chunk_len = buffer_len - total_read;

        switch (region) {
        case Region::kHeader: {
            const std::uint64_t rel = cur_offset - L.header_offset;
            const std::uint64_t region_remaining = kVhdxHeaderSize - rel;
            if (chunk_len > region_remaining) {
                chunk_len = static_cast<DWORD>(region_remaining);
            }
            std::memcpy(dst + total_read, header_region_.data() + rel, chunk_len);
            break;
        }
        case Region::kLog: {
            const std::uint64_t rel = cur_offset - L.log_offset;
            const std::uint64_t region_remaining = kVhdxLogSize - rel;
            if (chunk_len > region_remaining) {
                chunk_len = static_cast<DWORD>(region_remaining);
            }
            std::memcpy(dst + total_read, log_region_.data() + rel, chunk_len);
            break;
        }
        case Region::kMetadata: {
            const std::uint64_t rel = cur_offset - L.metadata_offset;
            const std::uint64_t region_remaining = L.bat_offset - cur_offset;
            if (chunk_len > region_remaining) {
                chunk_len = static_cast<DWORD>(region_remaining);
            }
            std::memcpy(dst + total_read, metadata_region_.data() + rel, chunk_len);
            break;
        }
        case Region::kBat: {
            const std::uint64_t rel = cur_offset - L.bat_offset;
            const std::uint64_t region_remaining = L.data_offset - cur_offset;
            if (chunk_len > region_remaining) {
                chunk_len = static_cast<DWORD>(region_remaining);
            }
            std::memcpy(dst + total_read, bat_region_.data() + rel, chunk_len);
            break;
        }
        case Region::kData: {
            std::uint64_t raw_offset = 0;
            std::uint64_t raw_remaining = 0;
            if (!to_raw_offset(cur_offset, &raw_offset, &raw_remaining)) {
                return STATUS_INVALID_PARAMETER;
            }
            if (raw_remaining == 0) {
                const std::uint64_t tail = L.vhdx_file_size - cur_offset;
                chunk_len = buffer_len - total_read;
                if (chunk_len > tail) {
                    chunk_len = static_cast<DWORD>(tail);
                }
                ZeroMemory(dst + total_read, chunk_len);
            } else {
                if (chunk_len > raw_remaining) {
                    chunk_len = static_cast<DWORD>(raw_remaining);
                }
                DWORD rd = 0;
                const NTSTATUS st = backing_.read(dst + total_read, chunk_len, &rd,
                                                  static_cast<LONGLONG>(raw_offset));
                if (st != STATUS_SUCCESS) {
                    return st;
                }
                if (rd < chunk_len) {
                    ZeroMemory(dst + total_read + rd, chunk_len - rd);
                }
            }
            break;
        }
        case Region::kOutOfRange:
            return STATUS_END_OF_FILE;
        }

        total_read += chunk_len;
        if (chunk_len == 0) {
            break;
        }
    }

    *bytes_read = total_read;
    return STATUS_SUCCESS;
}

NTSTATUS VhdxDiskImage::write_locked(std::uint64_t vhdx_offset, const void* buffer,
                                     DWORD bytes_to_write, LPDWORD bytes_written) {
    if (backing_.is_read_only()) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    const Layout& L = layout_;
    *bytes_written = 0;

    if (vhdx_offset >= L.vhdx_file_size) {
        return STATUS_END_OF_FILE;
    }

    const auto* src = static_cast<const std::uint8_t*>(buffer);
    DWORD total_written = 0;

    while (total_written < bytes_to_write) {
        const std::uint64_t cur_offset = vhdx_offset + total_written;
        const Region region = classify_offset(cur_offset);
        DWORD chunk_len = bytes_to_write - total_written;

        switch (region) {
        case Region::kHeader:
        case Region::kLog:
        case Region::kMetadata:
        case Region::kBat: {
            std::uint64_t region_end = L.data_offset;
            if (region == Region::kHeader) {
                region_end = L.log_offset;
            } else if (region == Region::kLog) {
                region_end = L.metadata_offset;
            } else if (region == Region::kMetadata) {
                region_end = L.bat_offset;
            }
            const std::uint64_t remaining = region_end - cur_offset;
            if (chunk_len > remaining) {
                chunk_len = static_cast<DWORD>(remaining);
            }
            break;
        }
        case Region::kData: {
            std::uint64_t raw_offset = 0;
            std::uint64_t raw_remaining = 0;
            if (!to_raw_offset(cur_offset, &raw_offset, &raw_remaining)) {
                return STATUS_INVALID_PARAMETER;
            }
            if (raw_remaining > 0) {
                if (chunk_len > raw_remaining) {
                    chunk_len = static_cast<DWORD>(raw_remaining);
                }
                DWORD wr = 0;
                const NTSTATUS st =
                    backing_.write(src + total_written, chunk_len, &wr,
                                   static_cast<LONGLONG>(raw_offset));
                if (st != STATUS_SUCCESS) {
                    return st;
                }
                chunk_len = wr;
            } else {
                const std::uint64_t tail = L.vhdx_file_size - cur_offset;
                if (chunk_len > tail) {
                    chunk_len = static_cast<DWORD>(tail);
                }
            }
            break;
        }
        case Region::kOutOfRange:
            return STATUS_END_OF_FILE;
        }

        total_written += chunk_len;
        if (chunk_len == 0) {
            break;
        }
    }

    *bytes_written = total_written;
    return STATUS_SUCCESS;
}

} // namespace aegra::adapters::dokan::detail
