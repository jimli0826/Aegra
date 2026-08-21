#pragma once

#include "aegra/adapters/ntfs/ntfs_reader.h"
#include "aegra/base/error.h"
#include "aegra/ntfs_core/attribute.h"
#include "aegra/ntfs_core/attribute_list.h"
#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/bitmap.h"
#include "aegra/ntfs_core/boot_sector.h"
#include "aegra/ntfs_core/fixup.h"
#include "aegra/ntfs_core/layout_read.h"
#include "aegra/ntfs_core/mft_record.h"
#include "aegra/ntfs_core/runlist.h"
#include "aegra/ntfs_core/types.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegra::adapters::ntfs::detail {

using ntfs_core::apply_fixup;
using ntfs_core::AttributeValue;
using ntfs_core::BootGeometry;
using ntfs_core::checked_add_u64;
using ntfs_core::checked_mul_u64;
using ntfs_core::DataRun;
using ntfs_core::FileReference;
using ntfs_core::kAttrAttributeList;
using ntfs_core::kAttrBitmap;
using ntfs_core::kAttrData;
using ntfs_core::kAttrEnd;
using ntfs_core::kAttrFileName;
using ntfs_core::kAttrIndexAllocation;
using ntfs_core::kAttrIndexRoot;
using ntfs_core::kAttrStandardInformation;
using ntfs_core::kFileAttrCompressed;
using ntfs_core::kFileAttrDirectory;
using ntfs_core::kFileAttrEncrypted;
using ntfs_core::kFileAttrHidden;
using ntfs_core::kFileAttrReparse;
using ntfs_core::kFileAttrSystem;
using ntfs_core::make_error;
using ntfs_core::pack_file_reference;
using ntfs_core::parse_attribute_list;
using ntfs_core::parse_boot_sector;
using ntfs_core::parse_mft_record_bytes;
using ntfs_core::ParsedMftRecord;
using ntfs_core::read_from_attribute;
using ntfs_core::read_u16;
using ntfs_core::read_u32;
using ntfs_core::read_u64;
using ntfs_core::unpack_file_reference;

// $FILE_NAME.flags / $I30 key flags (not Win32 FILE_ATTRIBUTE_*):
// DUP_FILE_NAME_INDEX_PRESENT — directory has a file-name index ($I30).
inline constexpr std::uint32_t kDupFileNameIndexPresent = 0x10000000U;
inline constexpr std::uint32_t kDupViewIndexPresent = 0x20000000U;

/// Maps NTFS $FILE_NAME flags (and optional MFT directory bit) to Win32-style attributes.
[[nodiscard]] inline void apply_file_name_flags(std::uint32_t& file_attributes,
                                                bool& is_directory) noexcept {
    if ((file_attributes & kDupFileNameIndexPresent) != 0 ||
        (file_attributes & kFileAttrDirectory) != 0) {
        is_directory = true;
        file_attributes |= kFileAttrDirectory;
    } else {
        is_directory = false;
    }
}

inline constexpr std::uint32_t kIndexEntryNode = 0x0001;
inline constexpr std::uint32_t kIndexEntryEnd = 0x0002;

inline constexpr std::uint8_t kFileNameWin32 = 1;
inline constexpr std::uint8_t kFileNameWin32AndDos = 3;

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

[[nodiscard]] base::Result<FileDataLayout>
build_file_layout(const ParsedMftRecord& record, const std::vector<ParsedMftRecord>& extensions);

[[nodiscard]] base::Result<std::vector<NtfsEntry>>
parse_index_entries(std::span<const std::byte> entry_area);

[[nodiscard]] base::Result<std::string> encode_continuation(std::uint32_t skip_count);

[[nodiscard]] base::Result<std::uint32_t>
decode_continuation(const std::optional<std::string>& token);

[[nodiscard]] NtfsFileReference to_explorer_reference(FileReference reference) noexcept;
[[nodiscard]] FileReference to_core_reference(NtfsFileReference reference) noexcept;
[[nodiscard]] NtfsVolumeInfo to_volume_info(const BootGeometry& geometry) noexcept;
[[nodiscard]] BootGeometry to_boot_geometry(const NtfsVolumeInfo& info) noexcept;

} // namespace aegra::adapters::ntfs::detail
