#pragma once

#include "aegra/adapters/ntfs/ntfs_reader.h"
#include "aegra/base/error.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegra::adapters::ntfs::detail {

inline constexpr std::uint32_t kAttrStandardInformation = 0x10;
inline constexpr std::uint32_t kAttrAttributeList = 0x20;
inline constexpr std::uint32_t kAttrFileName = 0x30;
inline constexpr std::uint32_t kAttrData = 0x80;
inline constexpr std::uint32_t kAttrIndexRoot = 0x90;
inline constexpr std::uint32_t kAttrIndexAllocation = 0xA0;
inline constexpr std::uint32_t kAttrBitmap = 0xB0;
inline constexpr std::uint32_t kAttrEnd = 0xFFFFFFFFU;

inline constexpr std::uint32_t kFileAttrReadonly = 0x0001;
inline constexpr std::uint32_t kFileAttrHidden = 0x0002;
inline constexpr std::uint32_t kFileAttrSystem = 0x0004;
inline constexpr std::uint32_t kFileAttrDirectory = 0x0010;
inline constexpr std::uint32_t kFileAttrArchive = 0x0020;
inline constexpr std::uint32_t kFileAttrCompressed = 0x0800;
inline constexpr std::uint32_t kFileAttrReparse = 0x0400;
inline constexpr std::uint32_t kFileAttrEncrypted = 0x4000;
// $FILE_NAME.flags / $I30 key flags (not Win32 FILE_ATTRIBUTE_*):
// DUP_FILE_NAME_INDEX_PRESENT — directory has a file-name index ($I30).
inline constexpr std::uint32_t kDupFileNameIndexPresent = 0x10000000U;
inline constexpr std::uint32_t kDupViewIndexPresent = 0x20000000U;

/// Maps NTFS $FILE_NAME flags (and optional MFT directory bit) to Win32-style attributes.
[[nodiscard]] inline void apply_file_name_flags(std::uint32_t& file_attributes,
                                                bool& is_directory) noexcept {
    // Observed on real volumes (and in backup ntfs_parser): directories carry
    // DUP_FILE_NAME_INDEX_PRESENT (0x10000000) rather than only FILE_ATTRIBUTE_DIRECTORY.
    if ((file_attributes & kDupFileNameIndexPresent) != 0 ||
        (file_attributes & kFileAttrDirectory) != 0) {
        is_directory = true;
        file_attributes |= kFileAttrDirectory;
    } else {
        is_directory = false;
    }
}

inline constexpr std::uint16_t kMftRecordInUse = 0x0001;
inline constexpr std::uint16_t kMftRecordIsDirectory = 0x0002;

inline constexpr std::uint32_t kIndexEntryNode = 0x0001;
inline constexpr std::uint32_t kIndexEntryEnd = 0x0002;

inline constexpr std::uint8_t kFileNameWin32 = 1;
inline constexpr std::uint8_t kFileNameWin32AndDos = 3;

[[nodiscard]] inline base::Error make_error(base::ErrorCode code, std::string message_code) {
    return {code, std::move(message_code)};
}

[[nodiscard]] bool checked_add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;
[[nodiscard]] bool checked_mul_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> data, std::size_t offset);
[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> data, std::size_t offset);
[[nodiscard]] std::uint64_t read_u64(std::span<const std::byte> data, std::size_t offset);
[[nodiscard]] std::int64_t read_signed_le(std::span<const std::byte> data, std::size_t offset,
                                          std::size_t length);

[[nodiscard]] base::Result<void>
apply_fixup(std::span<std::byte> buffer, std::uint32_t bytes_per_sector,
             std::uint16_t usa_offset, std::uint16_t usa_count);

struct DataRun final {
    std::uint64_t vcn{0};
    std::uint64_t lcn{0}; // meaningful when !sparse
    std::uint64_t cluster_count{0};
    bool sparse{false};
};

struct AttributeValue final {
    std::uint32_t type{0};
    std::u16string name;
    bool non_resident{false};
    bool compressed{false};
    bool encrypted{false};
    bool sparse{false};
    std::uint64_t data_size{0};
    std::uint64_t allocated_size{0};
    std::uint64_t initialized_size{0};
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

struct FileDataLayout final {
    bool is_directory{false};
    bool is_reparse{false};
    bool is_compressed{false};
    bool is_encrypted{false};
    std::uint32_t file_attributes{0};
    std::uint64_t logical_size{0};
    std::uint64_t allocated_size{0};
    std::uint64_t creation_time{0};
    std::uint64_t modification_time{0};
    std::uint64_t mft_change_time{0};
    std::uint64_t access_time{0};
    std::u16string best_name;
    std::uint64_t parent_record{0};
    std::uint16_t parent_sequence{0};
    AttributeValue unnamed_data;
    AttributeValue index_root;
    AttributeValue index_allocation;
    /// $BITMAP for the directory file-name index ($I30): one bit per index buffer.
    AttributeValue index_bitmap;
    bool has_unnamed_data{false};
    bool has_index_root{false};
    bool has_index_allocation{false};
    bool has_index_bitmap{false};
};

template <typename Key, typename Value>
class LruCache final {
  public:
    explicit LruCache(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    [[nodiscard]] std::optional<Value> try_get(const Key& key) {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return std::nullopt;
        }
        order_.splice(order_.begin(), order_, it->second);
        return it->second->second;
    }

    void put(Key key, Value value) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->second = std::move(value);
            order_.splice(order_.begin(), order_, it->second);
            return;
        }
        order_.emplace_front(std::move(key), std::move(value));
        index_[order_.front().first] = order_.begin();
        while (index_.size() > capacity_) {
            auto last = std::prev(order_.end());
            index_.erase(last->first);
            order_.pop_back();
        }
    }

    void clear() noexcept {
        index_.clear();
        order_.clear();
    }

  private:
    using List = std::list<std::pair<Key, Value>>;
    std::size_t capacity_;
    List order_;
    std::unordered_map<Key, typename List::iterator> index_;
};

[[nodiscard]] base::Result<NtfsVolumeInfo>
parse_boot_sector(std::span<const std::byte> sector);

[[nodiscard]] base::Result<std::vector<DataRun>>
parse_runlist(std::span<const std::byte> runlist, std::uint64_t first_vcn,
              std::uint64_t last_vcn);

[[nodiscard]] base::Result<void>
validate_attribute_header(std::span<const std::byte> record, std::size_t offset,
                          std::uint32_t& type, std::uint32_t& length, bool& non_resident);

[[nodiscard]] base::Result<AttributeValue>
parse_attribute(std::span<const std::byte> record, std::size_t offset);

[[nodiscard]] base::Result<ParsedMftRecord>
parse_mft_record_bytes(std::span<std::byte> record_bytes, std::uint32_t bytes_per_sector,
                       std::uint64_t expected_record_number);

[[nodiscard]] base::Result<FileDataLayout>
build_file_layout(const ParsedMftRecord& record,
                  const std::vector<ParsedMftRecord>& extensions);

[[nodiscard]] base::Result<std::size_t>
read_from_layout(ports::IRandomAccessReader& reader, const NtfsVolumeInfo& info,
                 const AttributeValue& data, std::uint64_t offset,
                 std::span<std::byte> destination, base::CancellationToken cancellation);

[[nodiscard]] base::Result<std::vector<NtfsEntry>>
parse_index_entries(std::span<const std::byte> entry_area);

[[nodiscard]] base::Result<std::string>
encode_continuation(std::uint32_t skip_count);

[[nodiscard]] base::Result<std::uint32_t>
decode_continuation(const std::optional<std::string>& token);

[[nodiscard]] NtfsFileReference unpack_file_reference(std::uint64_t packed) noexcept;
[[nodiscard]] std::uint64_t pack_file_reference(NtfsFileReference reference) noexcept;

} // namespace aegra::adapters::ntfs::detail
