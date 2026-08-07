#include "cow_backing_store.h"

#include "aegra/base/cancellation.h"

#include <winioctl.h>

#include <cstring>
#include <span>

namespace aegra::adapters::dokan::detail {
namespace {

HANDLE open_or_create_overlay(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }
    DWORD bytes_returned = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes_returned,
                    nullptr);
    return h;
}

} // namespace

CowBackingStore::CowBackingStore() = default;

CowBackingStore::~CowBackingStore() {
    if (overlay_ != INVALID_HANDLE_VALUE) {
        CloseHandle(overlay_);
        overlay_ = INVALID_HANDLE_VALUE;
    }
}

bool CowBackingStore::open_reader(ports::IRandomAccessReader* reader,
                                  const std::wstring& overlay_base, bool read_only) {
    if (reader == nullptr) {
        return false;
    }

    reader_ = reader;
    read_only_ = read_only;
    size_ = static_cast<LONGLONG>(reader->size_bytes());

    if (overlay_base.empty()) {
        return true;
    }

    overlay_path_ = overlay_base + L".overlay";
    map_path_ = overlay_base + L".overlay.map";
    overlay_ = open_or_create_overlay(overlay_path_);
    if (overlay_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    restore_bitmap();
    return true;
}

bool CowBackingStore::read_original_at(void* buffer, DWORD len, LONGLONG offset) {
    if (reader_ == nullptr) {
        std::memset(buffer, 0, len);
        return true;
    }

    auto* out = static_cast<std::uint8_t*>(buffer);
    const std::uint64_t disk_size = reader_->size_bytes();
    std::uint64_t src = static_cast<std::uint64_t>(offset);
    DWORD remaining = len;

    while (remaining > 0) {
        if (src >= disk_size) {
            std::memset(out, 0, remaining);
            break;
        }

        std::uint32_t want = remaining;
        if (src + want > disk_size) {
            want = static_cast<std::uint32_t>(disk_size - src);
        }

        auto span = std::span<std::byte>(reinterpret_cast<std::byte*>(out), want);
        auto result = reader_->read_at(src, span, base::CancellationToken{});
        if (!result || result.value() == 0) {
            std::memset(out, 0, remaining);
            break;
        }

        const auto got = static_cast<std::uint32_t>(result.value());
        out += got;
        src += got;
        remaining -= got;
    }
    return true;
}

void CowBackingStore::restore_bitmap() {
    if (map_path_.empty()) {
        return;
    }

    HANDLE map = CreateFileW(map_path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (map == INVALID_HANDLE_VALUE) {
        return;
    }

    LARGE_INTEGER map_size{};
    if (GetFileSizeEx(map, &map_size) && map_size.QuadPart > 0) {
        bitmap_.resize(static_cast<std::size_t>(map_size.QuadPart), 0);
        DWORD got = 0;
        ReadFile(map, bitmap_.data(), static_cast<DWORD>(bitmap_.size()), &got, nullptr);
    }
    CloseHandle(map);

    if (overlay_ != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER ov_size{};
        if (GetFileSizeEx(overlay_, &ov_size) && ov_size.QuadPart > size_) {
            size_ = ov_size.QuadPart;
        }
    }
}

bool CowBackingStore::block_present(std::uint64_t block_index) const {
    const std::size_t byte_idx = static_cast<std::size_t>(block_index >> 3);
    if (byte_idx >= bitmap_.size()) {
        return false;
    }
    return (bitmap_[byte_idx] & (1u << (block_index & 7))) != 0;
}

void CowBackingStore::ensure_bitmap_capacity(std::uint64_t block_count) {
    const std::size_t needed = static_cast<std::size_t>((block_count + 7) >> 3);
    if (bitmap_.size() < needed) {
        bitmap_.resize(needed, 0);
    }
}

void CowBackingStore::mark_present(std::uint64_t block_index) {
    ensure_bitmap_capacity(block_index + 1);
    bitmap_[static_cast<std::size_t>(block_index >> 3)] |=
        static_cast<std::uint8_t>(1u << (block_index & 7));
}

void CowBackingStore::flush_bitmap_locked() {
    if (map_path_.empty()) {
        return;
    }
    HANDLE h = CreateFileW(map_path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    if (!bitmap_.empty()) {
        WriteFile(h, bitmap_.data(), static_cast<DWORD>(bitmap_.size()), &written,
                  nullptr);
    }
    CloseHandle(h);
}

bool CowBackingStore::raw_read_at(HANDLE h, void* buffer, DWORD len, LONGLONG offset) {
    LARGE_INTEGER li{};
    li.QuadPart = offset;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        return false;
    }
    DWORD got = 0;
    if (!ReadFile(h, buffer, len, &got, nullptr)) {
        return false;
    }
    if (got < len) {
        std::memset(static_cast<std::uint8_t*>(buffer) + got, 0, len - got);
    }
    return true;
}

bool CowBackingStore::raw_write_at(HANDLE h, const void* buffer, DWORD len,
                                   LONGLONG offset, LPDWORD written) {
    LARGE_INTEGER li{};
    li.QuadPart = offset;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        return false;
    }
    return WriteFile(h, buffer, len, written, nullptr) != FALSE;
}

NTSTATUS CowBackingStore::read(void* buffer, DWORD buffer_len, LPDWORD bytes_read,
                               LONGLONG offset) {
    std::lock_guard lock(mutex_);

    auto* out = static_cast<std::uint8_t*>(buffer);
    LONGLONG cur = offset;
    DWORD remaining = buffer_len;

    while (remaining > 0) {
        const std::uint64_t block_index =
            static_cast<std::uint64_t>(cur) / kBlockSize;
        const std::uint64_t block_start = block_index * kBlockSize;
        const DWORD in_block_off = static_cast<DWORD>(cur - block_start);
        DWORD chunk = kBlockSize - in_block_off;
        if (chunk > remaining) {
            chunk = remaining;
        }

        const bool ok =
            (block_present(block_index) && overlay_ != INVALID_HANDLE_VALUE)
                ? raw_read_at(overlay_, out, chunk, cur)
                : read_original_at(out, chunk, cur);
        if (!ok) {
            return DokanNtStatusFromWin32(GetLastError());
        }

        out += chunk;
        cur += chunk;
        remaining -= chunk;
    }

    if (bytes_read) {
        *bytes_read = buffer_len;
    }
    return STATUS_SUCCESS;
}

NTSTATUS CowBackingStore::write(const void* buffer, DWORD bytes_to_write,
                                LPDWORD bytes_written, LONGLONG offset) {
    if (read_only_ || overlay_ == INVALID_HANDLE_VALUE) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    std::lock_guard lock(mutex_);

    const auto* in = static_cast<const std::uint8_t*>(buffer);
    LONGLONG cur = offset;
    DWORD remaining = bytes_to_write;
    static thread_local std::uint8_t block_buf[kBlockSize];

    while (remaining > 0) {
        const std::uint64_t block_index =
            static_cast<std::uint64_t>(cur) / kBlockSize;
        const std::uint64_t block_start = block_index * kBlockSize;
        const DWORD in_block_off = static_cast<DWORD>(cur - block_start);
        DWORD chunk = kBlockSize - in_block_off;
        if (chunk > remaining) {
            chunk = remaining;
        }

        DWORD written = 0;
        if (chunk == kBlockSize) {
            if (!raw_write_at(overlay_, in, chunk, static_cast<LONGLONG>(block_start),
                              &written)) {
                return DokanNtStatusFromWin32(GetLastError());
            }
        } else {
            const bool seeded =
                block_present(block_index)
                    ? raw_read_at(overlay_, block_buf, kBlockSize,
                                  static_cast<LONGLONG>(block_start))
                    : read_original_at(block_buf, kBlockSize,
                                       static_cast<LONGLONG>(block_start));
            if (!seeded) {
                return DokanNtStatusFromWin32(GetLastError());
            }
            std::memcpy(block_buf + in_block_off, in, chunk);
            if (!raw_write_at(overlay_, block_buf, kBlockSize,
                              static_cast<LONGLONG>(block_start), &written)) {
                return DokanNtStatusFromWin32(GetLastError());
            }
        }

        mark_present(block_index);
        in += chunk;
        cur += chunk;
        remaining -= chunk;
    }

    const LONGLONG end_pos = offset + bytes_to_write;
    if (end_pos > size_) {
        size_ = end_pos;
    }
    if (bytes_written) {
        *bytes_written = bytes_to_write;
    }
    return STATUS_SUCCESS;
}

void CowBackingStore::get_times(FILETIME* create, FILETIME* access, FILETIME* write) {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    if (create) {
        SystemTimeToFileTime(&st, create);
    }
    if (access) {
        SystemTimeToFileTime(&st, access);
    }
    if (write) {
        SystemTimeToFileTime(&st, write);
    }
}

void CowBackingStore::flush() {
    std::lock_guard lock(mutex_);
    if (overlay_ != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(overlay_);
    }
    flush_bitmap_locked();
}

NTSTATUS CowBackingStore::resize(LONGLONG raw_eof) {
    if (read_only_) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    std::lock_guard lock(mutex_);
    if (overlay_ != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER li{};
        li.QuadPart = raw_eof;
        if (!SetFilePointerEx(overlay_, li, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(overlay_)) {
            return DokanNtStatusFromWin32(GetLastError());
        }
    }
    size_ = raw_eof;
    return STATUS_SUCCESS;
}

void CowBackingStore::set_overlay_attributes(DWORD attributes) {
    std::lock_guard lock(mutex_);
    if (!overlay_path_.empty()) {
        SetFileAttributesW(overlay_path_.c_str(), attributes);
    }
}

void CowBackingStore::set_overlay_times(const FILETIME* creation,
                                        const FILETIME* last_access,
                                        const FILETIME* last_write) {
    std::lock_guard lock(mutex_);
    if (overlay_ != INVALID_HANDLE_VALUE) {
        SetFileTime(overlay_, creation, last_access, last_write);
    }
}

} // namespace aegra::adapters::dokan::detail
