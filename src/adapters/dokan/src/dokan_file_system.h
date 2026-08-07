// Dokan-facing virtual filesystem presenting one synthetic VHDX per disk.
// Trampolines recover the instance from DOKAN_OPTIONS::GlobalContext.
#pragma once

#include "aux_file_store.h"
#include "cow_backing_store.h"
#include "disk_image.h"
#include "dokan_ntstatus.h"

#include "aegra/ports/random_access.h"

#include <memory>
#include <string>
#include <vector>

namespace aegra::adapters::dokan::detail {

struct VirtualDiskEntry {
    std::wstring leaf_name;
    ports::IRandomAccessReader* reader{nullptr}; // non-owning; outlives session
    std::unique_ptr<CowBackingStore> backing;
    std::unique_ptr<DiskImage> disk;
    std::wstring overlay_base;
};

class DokanFileSystem final {
  public:
    DokanFileSystem(std::vector<VirtualDiskEntry> disks, bool read_only);
    ~DokanFileSystem();

    DokanFileSystem(const DokanFileSystem&) = delete;
    DokanFileSystem& operator=(const DokanFileSystem&) = delete;

    [[nodiscard]] bool is_read_only() const noexcept { return read_only_; }

    // Async mount via DokanCreateFileSystem. Returns a DokanMainResult code.
    [[nodiscard]] int mount(const std::wstring& mount_point);

    void close(DWORD close_timeout_ms = 15000);

    [[nodiscard]] bool is_running() const;
    [[nodiscard]] DWORD wait_closed(DWORD milliseconds = INFINITE) const;

    [[nodiscard]] DOKAN_HANDLE instance() const noexcept { return instance_; }
    [[nodiscard]] const std::wstring& mount_point() const noexcept {
        return mount_point_;
    }

  private:
    enum class NodeKind : ULONG64 {
        kNone = 0,
        kRootDir = 1,
        kDiskImage = 2,
        kAuxFile = 3,
        kAuxDir = 4,
    };

    static NodeKind kind(PDOKAN_FILE_INFO info) {
        return static_cast<NodeKind>(info->Context);
    }
    static void set_kind(PDOKAN_FILE_INFO info, NodeKind k) {
        info->Context = static_cast<ULONG64>(k);
    }

    [[nodiscard]] VirtualDiskEntry* find_disk(LPCWSTR path);
    [[nodiscard]] const VirtualDiskEntry* find_disk(LPCWSTR path) const;

    [[nodiscard]] NTSTATUS create_file(LPCWSTR file_name, ACCESS_MASK desired_access,
                                       ULONG file_attributes, ULONG share_access,
                                       ULONG create_disposition, ULONG create_options,
                                       PDOKAN_FILE_INFO info);
    void cleanup(LPCWSTR file_name, PDOKAN_FILE_INFO info);
    void close_file(LPCWSTR file_name, PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS read_file(LPCWSTR file_name, void* buffer, DWORD buffer_len,
                                     LPDWORD read_length, LONGLONG offset,
                                     PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS write_file(LPCWSTR file_name, const void* buffer,
                                      DWORD bytes_to_write, LPDWORD bytes_written,
                                      LONGLONG offset, PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS flush_file_buffers(LPCWSTR file_name,
                                              PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS get_file_information(LPCWSTR file_name,
                                                LPBY_HANDLE_FILE_INFORMATION buffer,
                                                PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS find_files(LPCWSTR file_name, PFillFindData fill_find_data,
                                      PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS set_file_attributes(LPCWSTR file_name,
                                               DWORD file_attributes,
                                               PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS set_file_time(LPCWSTR file_name, const FILETIME* creation,
                                         const FILETIME* last_access,
                                         const FILETIME* last_write,
                                         PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS delete_file(LPCWSTR file_name, PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS delete_directory(LPCWSTR file_name, PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS move_file(LPCWSTR file_name, LPCWSTR new_file_name,
                                     BOOL replace_if_existing, PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS set_end_of_file(LPCWSTR file_name, LONGLONG byte_offset,
                                           PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS set_allocation_size(LPCWSTR file_name, LONGLONG alloc_size,
                                               PDOKAN_FILE_INFO info);

    static DokanFileSystem* self(PDOKAN_FILE_INFO info);

    static NTSTATUS DOKAN_CALLBACK s_create_file(LPCWSTR, PDOKAN_IO_SECURITY_CONTEXT,
                                                 ACCESS_MASK, ULONG, ULONG, ULONG,
                                                 ULONG, PDOKAN_FILE_INFO);
    static void DOKAN_CALLBACK s_cleanup(LPCWSTR, PDOKAN_FILE_INFO);
    static void DOKAN_CALLBACK s_close_file(LPCWSTR, PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_read_file(LPCWSTR, LPVOID, DWORD, LPDWORD,
                                               LONGLONG, PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_write_file(LPCWSTR, LPCVOID, DWORD, LPDWORD,
                                                LONGLONG, PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_flush_file_buffers(LPCWSTR, PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_get_file_information(LPCWSTR,
                                                          LPBY_HANDLE_FILE_INFORMATION,
                                                          PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_find_files(LPCWSTR, PFillFindData,
                                                PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_set_file_attributes(LPCWSTR, DWORD,
                                                         PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_set_file_time(LPCWSTR, CONST FILETIME*,
                                                   CONST FILETIME*, CONST FILETIME*,
                                                   PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_delete_file(LPCWSTR, PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_delete_directory(LPCWSTR, PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_move_file(LPCWSTR, LPCWSTR, BOOL,
                                               PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_set_end_of_file(LPCWSTR, LONGLONG,
                                                     PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_set_allocation_size(LPCWSTR, LONGLONG,
                                                         PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_get_volume_information(LPWSTR, DWORD, LPDWORD,
                                                            LPDWORD, LPDWORD, LPWSTR,
                                                            DWORD, PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_get_disk_free_space(PULONGLONG, PULONGLONG,
                                                         PULONGLONG, PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_get_file_security(LPCWSTR, PSECURITY_INFORMATION,
                                                       PSECURITY_DESCRIPTOR, ULONG,
                                                       PULONG, PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_set_file_security(LPCWSTR, PSECURITY_INFORMATION,
                                                       PSECURITY_DESCRIPTOR, ULONG,
                                                       PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_mounted(LPCWSTR, PDOKAN_FILE_INFO);
    static NTSTATUS DOKAN_CALLBACK s_unmounted(PDOKAN_FILE_INFO);

    std::vector<VirtualDiskEntry> disks_;
    AuxFileStore aux_;
    bool read_only_{true};

    std::wstring mount_point_;
    std::wstring actual_mount_point_;
    DOKAN_OPTIONS options_{};
    DOKAN_OPERATIONS ops_{};
    DOKAN_HANDLE instance_{nullptr};
};

} // namespace aegra::adapters::dokan::detail
