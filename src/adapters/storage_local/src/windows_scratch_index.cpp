#include "windows_scratch_internal.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace aegra::adapters::storage_local::detail {
namespace {

[[nodiscard]] std::uint32_t read_le32(const std::byte* data) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[3])) << 24U);
}

[[nodiscard]] std::uint64_t read_le64(const std::byte* data) noexcept {
    return static_cast<std::uint64_t>(read_le32(data)) |
           (static_cast<std::uint64_t>(read_le32(data + 4)) << 32U);
}

void write_le32(std::byte* data, const std::uint32_t value) noexcept {
    data[0] = static_cast<std::byte>(value & 0xFFU);
    data[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    data[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    data[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

void write_le64(std::byte* data, const std::uint64_t value) noexcept {
    write_le32(data, static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
    write_le32(data + 4, static_cast<std::uint32_t>((value >> 32U) & 0xFFFFFFFFU));
}

[[nodiscard]] base::Result<void> write_idx_header(HANDLE handle, const std::uint64_t count) {
    std::byte header[kScratchIndexHeaderBytes]{};
    write_le32(header, kScratchIndexMagic);
    write_le32(header + 4, kScratchIndexVersion);
    write_le64(header + 8, count);
    return scratch_write_exact(handle, 0, header);
}

[[nodiscard]] base::Result<ScratchIndexRecord> read_disk_record(HANDLE handle,
                                                                const std::uint64_t index) {
    std::byte raw[kScratchIndexRecordBytes]{};
    const auto offset = kScratchIndexHeaderBytes + index * kScratchIndexRecordBytes;
    auto read = scratch_read_exact(handle, offset, raw);
    if (!read) {
        return base::Result<ScratchIndexRecord>::failure(read.error());
    }
    ScratchIndexRecord record;
    record.page_index = read_le64(raw);
    record.crc32 = read_le32(raw + 8);
    return base::Result<ScratchIndexRecord>::success(record);
}

[[nodiscard]] base::Result<void> write_disk_record(HANDLE handle, const std::uint64_t index,
                                                   const ScratchIndexRecord& record) {
    std::byte raw[kScratchIndexRecordBytes]{};
    write_le64(raw, record.page_index);
    write_le32(raw + 8, record.crc32);
    write_le32(raw + 12, 0);
    const auto offset = kScratchIndexHeaderBytes + index * kScratchIndexRecordBytes;
    return scratch_write_exact(handle, offset, raw);
}

} // namespace

ScratchPageIndex::ScratchPageIndex(std::wstring idx_path, const std::size_t memory_budget_bytes)
    : idx_path_(std::move(idx_path)), memory_budget_bytes_(memory_budget_bytes) {}

base::Result<void> ScratchPageIndex::initialize() {
    return open_idx(true);
}

std::size_t ScratchPageIndex::max_cache_entries() const noexcept {
    if (memory_budget_bytes_ < kScratchIndexEntryBytes) {
        return 0;
    }
    return memory_budget_bytes_ / kScratchIndexEntryBytes;
}

base::Result<void> ScratchPageIndex::open_idx(const bool create_if_missing) {
    if (idx_handle_.valid()) {
        return base::Result<void>::success();
    }
    const DWORD disposition = create_if_missing && !owns_idx_file_ ? CREATE_NEW : OPEN_EXISTING;
    UniqueHandle handle(CreateFileW(idx_path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    disposition, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        const auto error = GetLastError();
        if (!create_if_missing &&
            (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
            disk_record_count_ = 0;
            return base::Result<void>::success();
        }
        return base::Result<void>::failure(win32_error(error, "CreateFileW scratch idx"));
    }
    if (disposition == CREATE_NEW) {
        owns_idx_file_ = true;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size)) {
        return base::Result<void>::failure(win32_error(GetLastError(), "GetFileSizeEx scratch idx"));
    }
    if (size.QuadPart == 0) {
        auto header = write_idx_header(handle.get(), 0);
        if (!header) {
            return header;
        }
        disk_record_count_ = 0;
        idx_handle_ = std::move(handle);
        return base::Result<void>::success();
    }
    if (size.QuadPart < static_cast<LONGLONG>(kScratchIndexHeaderBytes)) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kCorruptData, "scratch idx header is truncated"));
    }
    std::byte header[kScratchIndexHeaderBytes]{};
    auto read = scratch_read_exact(handle.get(), 0, header);
    if (!read) {
        return read;
    }
    if (read_le32(header) != kScratchIndexMagic || read_le32(header + 4) != kScratchIndexVersion) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kCorruptData, "scratch idx header is invalid"));
    }
    disk_record_count_ = read_le64(header + 8);
    const auto expected = static_cast<std::uint64_t>(kScratchIndexHeaderBytes) +
                          disk_record_count_ * kScratchIndexRecordBytes;
    if (static_cast<std::uint64_t>(size.QuadPart) < expected) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kCorruptData, "scratch idx is truncated"));
    }
    idx_handle_ = std::move(handle);
    return base::Result<void>::success();
}

base::Result<std::optional<ScratchIndexRecord>>
ScratchPageIndex::disk_find(const std::uint64_t page_index) {
    auto opened = open_idx(false);
    if (!opened) {
        return base::Result<std::optional<ScratchIndexRecord>>::failure(opened.error());
    }
    if (!idx_handle_.valid() || disk_record_count_ == 0) {
        return base::Result<std::optional<ScratchIndexRecord>>::success(std::nullopt);
    }
    std::uint64_t low = 0;
    std::uint64_t high = disk_record_count_;
    while (low < high) {
        const std::uint64_t mid = low + (high - low) / 2U;
        auto record = read_disk_record(idx_handle_.get(), mid);
        if (!record) {
            return base::Result<std::optional<ScratchIndexRecord>>::failure(record.error());
        }
        if (record.value().page_index == page_index) {
            return base::Result<std::optional<ScratchIndexRecord>>::success(record.value());
        }
        if (record.value().page_index < page_index) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    return base::Result<std::optional<ScratchIndexRecord>>::success(std::nullopt);
}

void ScratchPageIndex::touch_locked(const std::uint64_t page_index) {
    auto it = cache_.find(page_index);
    if (it == cache_.end()) {
        return;
    }
    lru_.erase(it->second.lru);
    lru_.push_front(page_index);
    it->second.lru = lru_.begin();
}

base::Result<bool> ScratchPageIndex::contains(const std::uint64_t page_index) {
    if (closed_) {
        return base::Result<bool>::failure(
            local_error(base::ErrorCode::kConflict, "scratch store is closed"));
    }
    if (cache_.contains(page_index)) {
        touch_locked(page_index);
        return base::Result<bool>::success(true);
    }
    auto found = disk_find(page_index);
    if (!found) {
        return base::Result<bool>::failure(found.error());
    }
    return base::Result<bool>::success(found.value().has_value());
}

base::Result<std::uint32_t> ScratchPageIndex::get_crc(const std::uint64_t page_index) {
    if (closed_) {
        return base::Result<std::uint32_t>::failure(
            local_error(base::ErrorCode::kConflict, "scratch store is closed"));
    }
    if (const auto it = cache_.find(page_index); it != cache_.end()) {
        touch_locked(page_index);
        return base::Result<std::uint32_t>::success(it->second.crc32);
    }
    auto found = disk_find(page_index);
    if (!found) {
        return base::Result<std::uint32_t>::failure(found.error());
    }
    if (!found.value().has_value()) {
        return base::Result<std::uint32_t>::failure(
            local_error(base::ErrorCode::kNotFound, "scratch page is not written"));
    }
    return base::Result<std::uint32_t>::success(found.value()->crc32);
}

base::Result<void> ScratchPageIndex::put_crc(const std::uint64_t page_index,
                                             const std::uint32_t crc32) {
    if (closed_) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kConflict, "scratch store is closed"));
    }
    const bool in_cache = cache_.contains(page_index);
    if (!in_cache) {
        auto on_disk = disk_find(page_index);
        if (!on_disk) {
            return base::Result<void>::failure(on_disk.error());
        }
        if (!on_disk.value().has_value()) {
            ++written_page_count_;
        }
    }
    if (in_cache) {
        cache_[page_index].crc32 = crc32;
        touch_locked(page_index);
        return base::Result<void>::success();
    }
    lru_.push_front(page_index);
    cache_.emplace(page_index, CacheEntry{crc32, lru_.begin()});
    return spill_until_within_budget();
}

base::Result<void> ScratchPageIndex::spill_until_within_budget() {
    const auto limit = max_cache_entries();
    if (cache_.size() <= limit) {
        return base::Result<void>::success();
    }
    std::vector<ScratchIndexRecord> outgoing;
    std::vector<std::uint64_t> outgoing_pages;
    outgoing.reserve(cache_.size() - limit);
    outgoing_pages.reserve(cache_.size() - limit);
    for (auto it = lru_.rbegin(); it != lru_.rend() && cache_.size() - outgoing.size() > limit;
         ++it) {
        const auto page_it = cache_.find(*it);
        if (page_it == cache_.end()) {
            continue;
        }
        outgoing.push_back(ScratchIndexRecord{*it, page_it->second.crc32});
        outgoing_pages.push_back(*it);
    }
    if (outgoing.empty()) {
        return base::Result<void>::success();
    }
    std::sort(outgoing.begin(), outgoing.end(),
              [](const ScratchIndexRecord& left, const ScratchIndexRecord& right) {
                  return left.page_index < right.page_index;
              });
    auto merged = merge_records_to_disk(outgoing);
    if (!merged) {
        return merged;
    }
    for (const auto page_index : outgoing_pages) {
        const auto it = cache_.find(page_index);
        if (it == cache_.end()) {
            continue;
        }
        lru_.erase(it->second.lru);
        cache_.erase(it);
    }
    return base::Result<void>::success();
}

base::Result<void>
ScratchPageIndex::merge_records_to_disk(std::vector<ScratchIndexRecord> incoming) {
    auto opened = open_idx(true);
    if (!opened) {
        return opened;
    }
    const std::wstring temp_path = idx_path_ + L".tmp";
    UniqueHandle temp(CreateFileW(temp_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!temp.valid()) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "CreateFileW scratch idx temp"));
    }
    owns_temp_file_ = true;

    std::uint64_t out_index = 0;
    std::uint64_t disk_index = 0;
    std::size_t in_index = 0;
    auto write_one = [&](const ScratchIndexRecord& record) -> base::Result<void> {
        auto written = write_disk_record(temp.get(), out_index, record);
        if (!written) {
            return written;
        }
        ++out_index;
        return base::Result<void>::success();
    };

    while (disk_index < disk_record_count_ || in_index < incoming.size()) {
        ScratchIndexRecord disk_record{};
        bool have_disk = false;
        if (disk_index < disk_record_count_) {
            auto loaded = read_disk_record(idx_handle_.get(), disk_index);
            if (!loaded) {
                return base::Result<void>::failure(loaded.error());
            }
            disk_record = loaded.value();
            have_disk = true;
        }
        if (have_disk && in_index < incoming.size() &&
            incoming[in_index].page_index == disk_record.page_index) {
            auto written = write_one(incoming[in_index]);
            if (!written) {
                return written;
            }
            ++in_index;
            ++disk_index;
            continue;
        }
        if (have_disk && (in_index >= incoming.size() ||
                          disk_record.page_index < incoming[in_index].page_index)) {
            auto written = write_one(disk_record);
            if (!written) {
                return written;
            }
            ++disk_index;
            continue;
        }
        auto written = write_one(incoming[in_index]);
        if (!written) {
            return written;
        }
        ++in_index;
    }

    auto header = write_idx_header(temp.get(), out_index);
    if (!header) {
        return header;
    }
    if (!FlushFileBuffers(temp.get())) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "FlushFileBuffers scratch idx temp"));
    }
    temp.reset();
    idx_handle_.reset();
    if (!MoveFileExW(temp_path.c_str(), idx_path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "MoveFileExW scratch idx"));
    }
    owns_temp_file_ = false;
    disk_record_count_ = out_index;
    return open_idx(false);
}

base::Result<void> ScratchPageIndex::flush() {
    if (closed_) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kConflict, "scratch store is closed"));
    }
    if (!cache_.empty() && max_cache_entries() == 0) {
        auto spilled = spill_until_within_budget();
        if (!spilled) {
            return spilled;
        }
    }
    if (idx_handle_.valid() && !FlushFileBuffers(idx_handle_.get())) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "FlushFileBuffers scratch idx"));
    }
    return base::Result<void>::success();
}

void ScratchPageIndex::close() noexcept {
    closed_ = true;
    cache_.clear();
    lru_.clear();
    idx_handle_.reset();
}

base::Result<std::vector<std::uint64_t>>
ScratchPageIndex::written_pages_in_range(const std::uint64_t first_page,
                                         const std::uint64_t last_page) {
    if (closed_) {
        return base::Result<std::vector<std::uint64_t>>::failure(
            local_error(base::ErrorCode::kConflict, "scratch store is closed"));
    }
    if (first_page > last_page) {
        return base::Result<std::vector<std::uint64_t>>::success({});
    }
    std::vector<std::uint64_t> pages;
    for (const auto& [page_index, entry] : cache_) {
        (void)entry;
        if (page_index >= first_page && page_index <= last_page) {
            pages.push_back(page_index);
        }
    }
    auto opened = open_idx(false);
    if (!opened) {
        return base::Result<std::vector<std::uint64_t>>::failure(opened.error());
    }
    if (idx_handle_.valid() && disk_record_count_ > 0) {
        std::uint64_t low = 0;
        std::uint64_t high = disk_record_count_;
        while (low < high) {
            const std::uint64_t mid = low + (high - low) / 2U;
            auto record = read_disk_record(idx_handle_.get(), mid);
            if (!record) {
                return base::Result<std::vector<std::uint64_t>>::failure(record.error());
            }
            if (record.value().page_index < first_page) {
                low = mid + 1U;
            } else {
                high = mid;
            }
        }
        for (std::uint64_t index = low; index < disk_record_count_; ++index) {
            auto record = read_disk_record(idx_handle_.get(), index);
            if (!record) {
                return base::Result<std::vector<std::uint64_t>>::failure(record.error());
            }
            if (record.value().page_index > last_page) {
                break;
            }
            if (!cache_.contains(record.value().page_index)) {
                pages.push_back(record.value().page_index);
            }
        }
    }
    std::sort(pages.begin(), pages.end());
    return base::Result<std::vector<std::uint64_t>>::success(std::move(pages));
}

} // namespace aegra::adapters::storage_local::detail
