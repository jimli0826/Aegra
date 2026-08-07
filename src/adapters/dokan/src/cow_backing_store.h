// Copy-on-write backing store for the virtual disk data plane.
//
// Read-only original bytes come from ports::IRandomAccessReader (never modified).
// Optional overlay + bitmap support write paths; read-only mounts reject writes.
#pragma once

#include "dokan_ntstatus.h"

#include "aegra/ports/random_access.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace aegra::adapters::dokan::detail {

class CowBackingStore final {
  public:
    static constexpr std::uint32_t kBlockSize = 64 * 1024;

    CowBackingStore();
    ~CowBackingStore();

    CowBackingStore(const CowBackingStore&) = delete;
    CowBackingStore& operator=(const CowBackingStore&) = delete;

    // Open a read-only random-access source. overlay_base is used for optional
    // overlay/bitmap sidecar paths (created for structure; writes still rejected
    // when read_only is true).
    [[nodiscard]] bool open_reader(ports::IRandomAccessReader* reader,
                                   const std::wstring& overlay_base,
                                   bool read_only);

    [[nodiscard]] NTSTATUS read(void* buffer, DWORD buffer_len, LPDWORD bytes_read,
                                LONGLONG offset);
    [[nodiscard]] NTSTATUS write(const void* buffer, DWORD bytes_to_write,
                                 LPDWORD bytes_written, LONGLONG offset);

    void get_times(FILETIME* create, FILETIME* access, FILETIME* write);

    void flush();

    [[nodiscard]] NTSTATUS resize(LONGLONG raw_eof);

    void set_overlay_attributes(DWORD attributes);
    void set_overlay_times(const FILETIME* creation, const FILETIME* last_access,
                           const FILETIME* last_write);

    [[nodiscard]] LONGLONG size() const noexcept { return size_; }
    [[nodiscard]] bool is_read_only() const noexcept { return read_only_; }

  private:
    [[nodiscard]] bool block_present(std::uint64_t block_index) const;
    void ensure_bitmap_capacity(std::uint64_t block_count);
    void mark_present(std::uint64_t block_index);
    void flush_bitmap_locked();
    void restore_bitmap();

    [[nodiscard]] bool read_original_at(void* buffer, DWORD len, LONGLONG offset);

    [[nodiscard]] static bool raw_read_at(HANDLE h, void* buffer, DWORD len,
                                          LONGLONG offset);
    [[nodiscard]] static bool raw_write_at(HANDLE h, const void* buffer, DWORD len,
                                           LONGLONG offset, LPDWORD written);

    std::wstring overlay_path_;
    std::wstring map_path_;
    HANDLE overlay_{INVALID_HANDLE_VALUE};
    ports::IRandomAccessReader* reader_{nullptr};
    std::vector<std::uint8_t> bitmap_;
    mutable std::mutex mutex_;
    LONGLONG size_{0};
    bool read_only_{true};
};

} // namespace aegra::adapters::dokan::detail
