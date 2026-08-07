#include "disk_image.h"

#include "srw_lock.h"

namespace aegra::adapters::dokan::detail {

DiskImage::DiskImage(CowBackingStore& backing) : backing_(backing) {}

void DiskImage::rebuild() {
    ExclusiveSrwLock lock(layout_lock_);
    rebuild_locked();
}

NTSTATUS DiskImage::read(std::uint64_t offset, void* buffer, DWORD buffer_len,
                         LPDWORD bytes_read) {
    SharedSrwLock lock(layout_lock_);
    return read_locked(offset, buffer, buffer_len, bytes_read);
}

NTSTATUS DiskImage::write(std::uint64_t offset, const void* buffer,
                          DWORD bytes_to_write, LPDWORD bytes_written) {
    SharedSrwLock lock(layout_lock_);
    return write_locked(offset, buffer, bytes_to_write, bytes_written);
}

NTSTATUS DiskImage::set_end_of_file(std::uint64_t virtual_eof) {
    ExclusiveSrwLock lock(layout_lock_);

    const std::uint64_t data_offset = data_offset_locked();
    if (virtual_eof < data_offset) {
        return STATUS_SUCCESS;
    }

    const std::uint64_t raw_eof = virtual_eof - data_offset;
    const NTSTATUS st = backing_.resize(static_cast<LONGLONG>(raw_eof));
    if (st != STATUS_SUCCESS) {
        return st;
    }

    rebuild_locked();
    return STATUS_SUCCESS;
}

std::uint64_t DiskImage::file_size() const {
    SharedSrwLock lock(layout_lock_);
    return file_size_locked();
}

std::uint64_t DiskImage::data_offset() const {
    SharedSrwLock lock(layout_lock_);
    return data_offset_locked();
}

std::uint64_t DiskImage::unit_size() const {
    SharedSrwLock lock(layout_lock_);
    return unit_size_locked();
}

} // namespace aegra::adapters::dokan::detail
