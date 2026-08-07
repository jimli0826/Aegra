// Abstract base for a synthetic virtual disk image (VHDX).
//
// Lock ordering: layout SRWLOCK is always taken before the backing store lock.
#pragma once

#include "cow_backing_store.h"
#include "dokan_ntstatus.h"

#include <cstdint>

namespace aegra::adapters::dokan::detail {

namespace disk_names {
inline constexpr const wchar_t* kVhdx = L"disk.vhdx";
inline constexpr const wchar_t* kVhdxPath = L"\\disk.vhdx";
} // namespace disk_names

enum class DiskFormat { kVhdx };

class DiskImage {
  public:
    explicit DiskImage(CowBackingStore& backing);
    virtual ~DiskImage() = default;

    DiskImage(const DiskImage&) = delete;
    DiskImage& operator=(const DiskImage&) = delete;

    void rebuild();

    [[nodiscard]] NTSTATUS read(std::uint64_t offset, void* buffer, DWORD buffer_len,
                                LPDWORD bytes_read);
    [[nodiscard]] NTSTATUS write(std::uint64_t offset, const void* buffer,
                                 DWORD bytes_to_write, LPDWORD bytes_written);
    [[nodiscard]] NTSTATUS set_end_of_file(std::uint64_t virtual_eof);

    [[nodiscard]] std::uint64_t file_size() const;
    [[nodiscard]] std::uint64_t data_offset() const;
    [[nodiscard]] std::uint64_t unit_size() const;

    [[nodiscard]] virtual const wchar_t* file_name() const = 0;

  protected:
    virtual void rebuild_locked() = 0;
    [[nodiscard]] virtual NTSTATUS read_locked(std::uint64_t offset, void* buffer,
                                               DWORD buffer_len,
                                               LPDWORD bytes_read) = 0;
    [[nodiscard]] virtual NTSTATUS write_locked(std::uint64_t offset,
                                                const void* buffer,
                                                DWORD bytes_to_write,
                                                LPDWORD bytes_written) = 0;
    [[nodiscard]] virtual std::uint64_t file_size_locked() const = 0;
    [[nodiscard]] virtual std::uint64_t data_offset_locked() const = 0;
    [[nodiscard]] virtual std::uint64_t unit_size_locked() const = 0;

    CowBackingStore& backing_;
    mutable SRWLOCK layout_lock_ = SRWLOCK_INIT;
};

} // namespace aegra::adapters::dokan::detail
