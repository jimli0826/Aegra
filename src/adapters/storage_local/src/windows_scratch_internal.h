#pragma once

#include "local_storage_internal.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::adapters::storage_local::detail {

inline constexpr std::uint32_t kScratchPageSize = 65536U;
inline constexpr std::size_t kScratchIndexEntryBytes = 64U;
inline constexpr std::uint32_t kScratchIndexMagic = 0x58495341U; // 'ASIX' LE
inline constexpr std::uint32_t kScratchIndexVersion = 1U;
inline constexpr std::size_t kScratchIndexHeaderBytes = 16U; // magic, version, count
inline constexpr std::size_t kScratchIndexRecordBytes = 16U; // page, crc, reserved

struct ScratchIndexRecord final {
    std::uint64_t page_index{0};
    std::uint32_t crc32{0};
};

[[nodiscard]] std::uint32_t scratch_crc32_ieee(std::span<const std::byte> data) noexcept;

[[nodiscard]] base::Result<std::wstring> scratch_utf8_to_wide(std::string_view utf8);
[[nodiscard]] base::Result<std::string> scratch_wide_to_utf8(std::wstring_view wide);

[[nodiscard]] bool scratch_equals_ignore_case(std::wstring_view left,
                                              std::wstring_view right) noexcept;

[[nodiscard]] base::Result<std::wstring>
scratch_resolve_absolute_local_path(std::string_view path_utf8);

[[nodiscard]] base::Result<void> scratch_require_parent_directory(const std::wstring& path);

[[nodiscard]] base::Result<void>
scratch_reject_forbidden_volume(const std::wstring& absolute_path,
                                std::string_view forbidden_volume_guid_utf8);

[[nodiscard]] base::Result<UniqueHandle>
scratch_create_sparse_file(const std::wstring& path, std::uint64_t logical_size_bytes);

[[nodiscard]] base::Result<void>
scratch_read_exact(HANDLE handle, std::uint64_t offset, std::span<std::byte> destination);

[[nodiscard]] base::Result<void>
scratch_write_exact(HANDLE handle, std::uint64_t offset, std::span<const std::byte> source);

/// Bounded page-index cache with sorted on-disk `.idx` spill (O(log n) lookups).
class ScratchPageIndex final {
  public:
    ScratchPageIndex(std::wstring idx_path, std::size_t memory_budget_bytes);

    [[nodiscard]] std::uint64_t written_page_count() const noexcept { return written_page_count_; }
    [[nodiscard]] const std::wstring& idx_path() const noexcept { return idx_path_; }
    [[nodiscard]] bool owns_idx_file() const noexcept { return owns_idx_file_; }
    [[nodiscard]] bool owns_temp_file() const noexcept { return owns_temp_file_; }

    [[nodiscard]] base::Result<void> initialize();
    [[nodiscard]] base::Result<bool> contains(std::uint64_t page_index);
    [[nodiscard]] base::Result<std::uint32_t> get_crc(std::uint64_t page_index);
    [[nodiscard]] base::Result<void> put_crc(std::uint64_t page_index, std::uint32_t crc32);
    [[nodiscard]] base::Result<void> flush();
    void close() noexcept;

    /// Collect written page indices overlapping [first_page, last_page] inclusive.
    [[nodiscard]] base::Result<std::vector<std::uint64_t>>
    written_pages_in_range(std::uint64_t first_page, std::uint64_t last_page);

  private:
    struct CacheEntry final {
        std::uint32_t crc32{0};
        std::list<std::uint64_t>::iterator lru{};
    };

    [[nodiscard]] std::size_t max_cache_entries() const noexcept;
    [[nodiscard]] base::Result<void> open_idx(bool create_if_missing);
    [[nodiscard]] base::Result<std::optional<ScratchIndexRecord>>
    disk_find(std::uint64_t page_index);
    [[nodiscard]] base::Result<void> spill_until_within_budget();
    [[nodiscard]] base::Result<void>
    merge_records_to_disk(std::vector<ScratchIndexRecord> incoming);
    void touch_locked(std::uint64_t page_index);

    std::wstring idx_path_;
    std::size_t memory_budget_bytes_{0};
    UniqueHandle idx_handle_;
    std::uint64_t disk_record_count_{0};
    std::uint64_t written_page_count_{0};
    std::map<std::uint64_t, CacheEntry> cache_;
    std::list<std::uint64_t> lru_;
    bool owns_idx_file_{false};
    bool owns_temp_file_{false};
    bool closed_{false};
};

} // namespace aegra::adapters::storage_local::detail
