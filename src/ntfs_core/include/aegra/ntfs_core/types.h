#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aegra::ntfs_core {

// Product/parse ceilings shared by codecs (ADR-0023 / ADR-0025).
inline constexpr std::uint32_t kMaxMftRecordBytes = 64U * 1024U;
inline constexpr std::uint32_t kMaxIndexRecordBytes = 1U * 1024U * 1024U;
inline constexpr std::size_t kMaxDataRuns = 65536;
inline constexpr std::size_t kMaxAttributesPerRecord = 1024;
inline constexpr std::size_t kMaxAttributeListEntries = 4096;
inline constexpr std::size_t kMaxRunlistEncodedBytes = 16U * 1024U * 1024U;

inline constexpr std::uint32_t kAttrStandardInformation = 0x10;
inline constexpr std::uint32_t kAttrAttributeList = 0x20;
inline constexpr std::uint32_t kAttrFileName = 0x30;
inline constexpr std::uint32_t kAttrData = 0x80;
inline constexpr std::uint32_t kAttrIndexRoot = 0x90;
inline constexpr std::uint32_t kAttrIndexAllocation = 0xA0;
inline constexpr std::uint32_t kAttrBitmap = 0xB0;
inline constexpr std::uint32_t kAttrEnd = 0xFFFFFFFFU;

inline constexpr std::uint16_t kMftRecordInUse = 0x0001;
inline constexpr std::uint16_t kMftRecordIsDirectory = 0x0002;

inline constexpr std::uint32_t kFileAttrReadonly = 0x0001;
inline constexpr std::uint32_t kFileAttrHidden = 0x0002;
inline constexpr std::uint32_t kFileAttrSystem = 0x0004;
inline constexpr std::uint32_t kFileAttrDirectory = 0x0010;
inline constexpr std::uint32_t kFileAttrArchive = 0x0020;
inline constexpr std::uint32_t kFileAttrReparse = 0x0400;
inline constexpr std::uint32_t kFileAttrCompressed = 0x0800;
inline constexpr std::uint32_t kFileAttrEncrypted = 0x4000;

/// Virtual cluster number (file-relative).
struct VirtualClusterNumber final {
    std::uint64_t value{0};

    [[nodiscard]] bool operator==(const VirtualClusterNumber&) const noexcept = default;
};

/// Logical cluster number on the volume.
struct LogicalClusterNumber final {
    std::uint64_t value{0};

    [[nodiscard]] bool operator==(const LogicalClusterNumber&) const noexcept = default;
};

struct ClusterCount final {
    std::uint64_t value{0};

    [[nodiscard]] bool operator==(const ClusterCount&) const noexcept = default;
};

struct ByteOffset final {
    std::uint64_t value{0};

    [[nodiscard]] bool operator==(const ByteOffset&) const noexcept = default;
};

struct ByteCount final {
    std::uint64_t value{0};

    [[nodiscard]] bool operator==(const ByteCount&) const noexcept = default;
};

/// MFT file reference: low 48 bits = record number, high 16 bits = sequence.
struct FileReference final {
    std::uint64_t record_number{0};
    std::uint16_t sequence_number{0};

    [[nodiscard]] bool operator==(const FileReference&) const noexcept = default;
};

struct BootGeometry final {
    std::uint32_t bytes_per_sector{0};
    std::uint32_t sectors_per_cluster{0};
    std::uint32_t bytes_per_cluster{0};
    std::uint32_t bytes_per_mft_record{0};
    std::uint32_t bytes_per_index_record{0};
    std::uint64_t total_clusters{0};
    // Bytes covered by BPB TotalSectors. The raw NTFS device additionally contains the
    // backup Boot Sector at this byte offset.
    ByteCount volume_size_bytes{};
    LogicalClusterNumber mft_start_lcn{};
    LogicalClusterNumber mft_mirror_start_lcn{};
    std::uint64_t volume_serial{0};
};

struct DataRun final {
    VirtualClusterNumber first_vcn{};
    LogicalClusterNumber first_lcn{}; // meaningful when !sparse
    ClusterCount cluster_count{};
    bool sparse{false};
};

struct AttributeValue final {
    std::uint32_t type{0};
    std::uint32_t record_offset{0};
    std::uint32_t attribute_length{0};
    std::uint16_t runlist_offset{0};
    std::u16string name;
    std::uint16_t attribute_id{0};
    bool non_resident{false};
    bool compressed{false};
    bool encrypted{false};
    bool sparse{false};
    ByteCount data_size{};
    ByteCount allocated_size{};
    ByteCount initialized_size{};
    std::vector<std::byte> resident_data;
    std::vector<DataRun> runs;
};

struct ParsedMftRecord final {
    std::uint64_t record_number{0};
    std::uint16_t sequence_number{0};
    bool in_use{false};
    bool is_directory{false};
    std::uint64_t base_record{0};
    std::vector<AttributeValue> attributes;
};

struct AttributeListEntry final {
    std::uint32_t type{0};
    std::u16string name;
    VirtualClusterNumber start_vcn{};
    FileReference attribute_record{};
    std::uint16_t attribute_id{0};
};

[[nodiscard]] bool is_known_attribute_type(std::uint32_t type) noexcept;

} // namespace aegra::ntfs_core
