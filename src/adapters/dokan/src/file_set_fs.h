// Dokan-facing read-only filesystem presenting a file_set Recovery Point
// namespace (IFileRecoveryPointReader) directly under a drive letter.
// Trampolines recover the instance from DOKAN_OPTIONS::GlobalContext.
#pragma once

#include "dokan_ntstatus.h"

#include "aegra/contracts/file_set.h"
#include "aegra/ports/file_recovery_point.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegra::adapters::dokan::detail {

class FileSetFileSystem final {
  public:
    // reader is non-owning and must outlive the mount session until close() returns.
    // The reader is not thread-safe; all access is serialized internally.
    explicit FileSetFileSystem(ports::IFileRecoveryPointReader& reader);
    ~FileSetFileSystem();

    FileSetFileSystem(const FileSetFileSystem&) = delete;
    FileSetFileSystem& operator=(const FileSetFileSystem&) = delete;

    // Async mount via DokanCreateFileSystem. Returns a DokanMainResult code.
    [[nodiscard]] int mount(const std::wstring& mount_point);

    void close(DWORD close_timeout_ms = 15000);

    [[nodiscard]] bool is_running() const;
    [[nodiscard]] const std::wstring& mount_point() const noexcept {
        return mount_point_;
    }
    // Mount-manager may pick a different letter; empty until the Mounted callback.
    [[nodiscard]] const std::wstring& actual_mount_point() const noexcept {
        return actual_mount_point_;
    }

  private:
    struct Node {
        contracts::FileEntryDesc desc;
        std::wstring name; // decoded UTF-16 leaf name (sanitized)
        bool children_loaded{false};
        std::unordered_map<std::wstring, std::uint64_t> children_by_lower_name;
        std::vector<std::uint64_t> child_ids; // stable enumeration order
    };

    // All lookups require mutex_ (reader access + cache mutation).
    [[nodiscard]] Node* node_for_locked(std::uint64_t entry_id);
    [[nodiscard]] NTSTATUS resolve_path_locked(LPCWSTR path, Node** out);
    [[nodiscard]] NTSTATUS ensure_children_locked(Node& directory);

    [[nodiscard]] NTSTATUS create_file(LPCWSTR file_name, ACCESS_MASK desired_access,
                                       ULONG create_disposition, ULONG create_options,
                                       PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS read_file(LPCWSTR file_name, void* buffer, DWORD buffer_len,
                                     LPDWORD read_length, LONGLONG offset,
                                     PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS get_file_information(LPCWSTR file_name,
                                                LPBY_HANDLE_FILE_INFORMATION buffer,
                                                PDOKAN_FILE_INFO info);
    [[nodiscard]] NTSTATUS find_files(LPCWSTR file_name, PFillFindData fill_find_data,
                                      PDOKAN_FILE_INFO info);

    static FileSetFileSystem* self(PDOKAN_FILE_INFO info);

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

    ports::IFileRecoveryPointReader* reader_{nullptr};
    std::mutex mutex_;
    // Index cache is immutable for the session (archive index is stable for the
    // reader lifetime), so cached nodes never expire.
    std::unordered_map<std::uint64_t, std::unique_ptr<Node>> nodes_;

    std::wstring mount_point_;
    std::wstring actual_mount_point_;
    DOKAN_OPTIONS options_{};
    DOKAN_OPERATIONS ops_{};
    DOKAN_HANDLE instance_{nullptr};
};

} // namespace aegra::adapters::dokan::detail
