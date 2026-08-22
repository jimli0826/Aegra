#include "dokan_file_system.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <thread>

namespace aegra::adapters::dokan::detail {
namespace {

std::mutex g_dokan_init_mu;
int g_dokan_init_count = 0;

bool is_drive_letter_mount_point(const std::wstring& mount_point) {
    if (mount_point.size() == 1) {
        return iswalpha(mount_point[0]) != 0;
    }
    if (mount_point.size() == 2) {
        return iswalpha(mount_point[0]) != 0 && mount_point[1] == L':';
    }
    return mount_point.size() == 3 && iswalpha(mount_point[0]) != 0 &&
           mount_point[1] == L':' &&
           (mount_point[2] == L'\\' || mount_point[2] == L'/');
}

} // namespace

void acquire_dokan_library() {
    std::lock_guard lock(g_dokan_init_mu);
    if (g_dokan_init_count++ == 0) {
        DokanInit();
    }
}

void release_dokan_library() {
    std::lock_guard lock(g_dokan_init_mu);
    if (g_dokan_init_count > 0 && --g_dokan_init_count == 0) {
        DokanShutdown();
    }
}

bool request_dokan_unmount(const std::wstring& mount_point) {
    if (mount_point.empty()) {
        return false;
    }

    std::wstring base = mount_point;
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
        base.pop_back();
    }
    if (base.empty()) {
        return false;
    }

    std::wstring candidates[2];
    if (is_drive_letter_mount_point(base) ||
        (base.size() == 1 && iswalpha(base[0]))) {
        const wchar_t letter = towupper(base[0]);
        candidates[0] = std::wstring(1, letter) + L':' + L'\\';
        candidates[1] = std::wstring(1, letter) + L':';
    } else {
        candidates[0] = base + L'\\';
        candidates[1] = base;
    }

    for (const auto& mp : candidates) {
        if (DokanRemoveMountPoint(mp.c_str())) {
            return true;
        }
    }
    return false;
}

void fill_dot_entries(PFillFindData fill_find_data, PDOKAN_FILE_INFO info) {
    WIN32_FIND_DATAW find_data{};
    find_data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    find_data.cFileName[0] = L'.';
    fill_find_data(&find_data, info);

    ZeroMemory(&find_data, sizeof(find_data));
    find_data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    find_data.cFileName[0] = L'.';
    find_data.cFileName[1] = L'.';
    fill_find_data(&find_data, info);
}

DokanFileSystem::DokanFileSystem(std::vector<VirtualDiskEntry> disks, bool read_only)
    : disks_(std::move(disks)), read_only_(read_only) {}

DokanFileSystem::~DokanFileSystem() { close(); }

VirtualDiskEntry* DokanFileSystem::find_disk(LPCWSTR path) {
    if (!path || path[0] != L'\\') {
        return nullptr;
    }
    for (auto& entry : disks_) {
        if (_wcsicmp(path + 1, entry.leaf_name.c_str()) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

const VirtualDiskEntry* DokanFileSystem::find_disk(LPCWSTR path) const {
    return const_cast<DokanFileSystem*>(this)->find_disk(path);
}

DokanFileSystem* DokanFileSystem::self(PDOKAN_FILE_INFO info) {
    return reinterpret_cast<DokanFileSystem*>(info->DokanOptions->GlobalContext);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_create_file(
    LPCWSTR file_name, PDOKAN_IO_SECURITY_CONTEXT security_context,
    ACCESS_MASK desired_access, ULONG file_attributes, ULONG share_access,
    ULONG create_disposition, ULONG create_options, PDOKAN_FILE_INFO info) {
    UNREFERENCED_PARAMETER(security_context);
    return self(info)->create_file(file_name, desired_access, file_attributes,
                                   share_access, create_disposition, create_options,
                                   info);
}

void DOKAN_CALLBACK DokanFileSystem::s_cleanup(LPCWSTR file_name,
                                               PDOKAN_FILE_INFO info) {
    self(info)->cleanup(file_name, info);
}

void DOKAN_CALLBACK DokanFileSystem::s_close_file(LPCWSTR file_name,
                                                  PDOKAN_FILE_INFO info) {
    self(info)->close_file(file_name, info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_read_file(LPCWSTR file_name, LPVOID buffer,
                                                     DWORD buffer_len,
                                                     LPDWORD read_length,
                                                     LONGLONG offset,
                                                     PDOKAN_FILE_INFO info) {
    return self(info)->read_file(file_name, buffer, buffer_len, read_length, offset,
                                 info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_write_file(LPCWSTR file_name,
                                                      LPCVOID buffer,
                                                      DWORD bytes_to_write,
                                                      LPDWORD bytes_written,
                                                      LONGLONG offset,
                                                      PDOKAN_FILE_INFO info) {
    return self(info)->write_file(file_name, buffer, bytes_to_write, bytes_written,
                                  offset, info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_flush_file_buffers(LPCWSTR file_name,
                                                              PDOKAN_FILE_INFO info) {
    return self(info)->flush_file_buffers(file_name, info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_get_file_information(
    LPCWSTR file_name, LPBY_HANDLE_FILE_INFORMATION buffer, PDOKAN_FILE_INFO info) {
    return self(info)->get_file_information(file_name, buffer, info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_find_files(LPCWSTR file_name,
                                                      PFillFindData fill_find_data,
                                                      PDOKAN_FILE_INFO info) {
    return self(info)->find_files(file_name, fill_find_data, info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_set_file_attributes(
    LPCWSTR file_name, DWORD file_attributes, PDOKAN_FILE_INFO info) {
    return self(info)->set_file_attributes(file_name, file_attributes, info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_set_file_time(LPCWSTR file_name,
                                                         CONST FILETIME* creation,
                                                         CONST FILETIME* last_access,
                                                         CONST FILETIME* last_write,
                                                         PDOKAN_FILE_INFO info) {
    return self(info)->set_file_time(file_name, creation, last_access, last_write,
                                     info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_delete_file(LPCWSTR file_name,
                                                       PDOKAN_FILE_INFO info) {
    return self(info)->delete_file(file_name, info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_delete_directory(LPCWSTR file_name,
                                                            PDOKAN_FILE_INFO info) {
    return self(info)->delete_directory(file_name, info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_move_file(LPCWSTR file_name,
                                                     LPCWSTR new_file_name,
                                                     BOOL replace_if_existing,
                                                     PDOKAN_FILE_INFO info) {
    return self(info)->move_file(file_name, new_file_name, replace_if_existing, info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_set_end_of_file(LPCWSTR file_name,
                                                           LONGLONG byte_offset,
                                                           PDOKAN_FILE_INFO info) {
    return self(info)->set_end_of_file(file_name, byte_offset, info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_set_allocation_size(LPCWSTR file_name,
                                                               LONGLONG alloc_size,
                                                               PDOKAN_FILE_INFO info) {
    return self(info)->set_allocation_size(file_name, alloc_size, info);
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_get_volume_information(
    LPWSTR volume_name_buffer, DWORD volume_name_size, LPDWORD volume_serial_number,
    LPDWORD maximum_component_length, LPDWORD file_system_flags,
    LPWSTR file_system_name_buffer, DWORD file_system_name_size,
    PDOKAN_FILE_INFO info) {
    UNREFERENCED_PARAMETER(info);
    wcscpy_s(volume_name_buffer, volume_name_size, L"Aegra Virtual Disk");
    *volume_serial_number = 0x41454752; // "AEGR"
    *maximum_component_length = 255;
    *file_system_flags = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
                         FILE_UNICODE_ON_DISK;
    if (file_system_name_size > 0) {
        wcscpy_s(file_system_name_buffer, file_system_name_size, L"NTFS");
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_get_disk_free_space(
    PULONGLONG free_bytes_available, PULONGLONG total_number_of_bytes,
    PULONGLONG total_number_of_free_bytes, PDOKAN_FILE_INFO info) {
    DokanFileSystem* fs = self(info);
    ULONGLONG total = 0;
    for (const auto& entry : fs->disks_) {
        if (entry.backing) {
            const LONGLONG size = entry.backing->size();
            if (size > 0) {
                total += static_cast<ULONGLONG>(size);
            }
        }
    }
    if (total == 0) {
        total = 1024ULL * 1024 * 1024;
    }
    *total_number_of_bytes = total;
    *total_number_of_free_bytes = 0;
    *free_bytes_available = 0;
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_get_file_security(LPCWSTR,
                                                             PSECURITY_INFORMATION,
                                                             PSECURITY_DESCRIPTOR,
                                                             ULONG, PULONG,
                                                             PDOKAN_FILE_INFO) {
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_set_file_security(LPCWSTR,
                                                             PSECURITY_INFORMATION,
                                                             PSECURITY_DESCRIPTOR,
                                                             ULONG, PDOKAN_FILE_INFO) {
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_mounted(LPCWSTR mount_point,
                                                   PDOKAN_FILE_INFO info) {
    if (info && mount_point) {
        if (DokanFileSystem* fs = self(info)) {
            fs->actual_mount_point_ = mount_point;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK DokanFileSystem::s_unmounted(PDOKAN_FILE_INFO info) {
    UNREFERENCED_PARAMETER(info);
    return STATUS_SUCCESS;
}

NTSTATUS DokanFileSystem::create_file(LPCWSTR file_name, ACCESS_MASK desired_access,
                                      ULONG file_attributes, ULONG share_access,
                                      ULONG create_disposition, ULONG create_options,
                                      PDOKAN_FILE_INFO info) {
    UNREFERENCED_PARAMETER(desired_access);
    UNREFERENCED_PARAMETER(file_attributes);
    UNREFERENCED_PARAMETER(share_access);

    if (wcscmp(file_name, L"\\") == 0) {
        info->IsDirectory = TRUE;
        set_kind(info, NodeKind::kRootDir);
        return STATUS_SUCCESS;
    }

    if (find_disk(file_name) != nullptr) {
        info->IsDirectory = FALSE;
        set_kind(info, NodeKind::kDiskImage);
        return STATUS_SUCCESS;
    }

    if (read_only_) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    bool is_directory = false;
    const NTSTATUS status =
        aux_.create(file_name, create_disposition, create_options, &is_directory);
    if (status != STATUS_SUCCESS) {
        return status;
    }
    info->IsDirectory = is_directory ? TRUE : FALSE;
    set_kind(info, is_directory ? NodeKind::kAuxDir : NodeKind::kAuxFile);
    return STATUS_SUCCESS;
}

void DokanFileSystem::cleanup(LPCWSTR file_name, PDOKAN_FILE_INFO info) {
    const NodeKind k = kind(info);
    if (k == NodeKind::kAuxFile || k == NodeKind::kAuxDir) {
        aux_.cleanup(file_name, info->DeletePending != FALSE);
    }
}

void DokanFileSystem::close_file(LPCWSTR file_name, PDOKAN_FILE_INFO info) {
    UNREFERENCED_PARAMETER(file_name);
    info->Context = 0;
}

NTSTATUS DokanFileSystem::read_file(LPCWSTR file_name, void* buffer, DWORD buffer_len,
                                    LPDWORD read_length, LONGLONG offset,
                                    PDOKAN_FILE_INFO info) {
    __try {
        const NodeKind k = kind(info);
        if (k == NodeKind::kAuxFile) {
            return aux_.read(file_name, buffer, buffer_len, read_length, offset);
        }
        if (k != NodeKind::kDiskImage) {
            return STATUS_INVALID_PARAMETER;
        }
        VirtualDiskEntry* entry = find_disk(file_name);
        if (!entry || !entry->disk) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        return entry->disk->read(static_cast<std::uint64_t>(offset), buffer,
                                 buffer_len, read_length);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (read_length) {
            *read_length = 0;
        }
        return STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS DokanFileSystem::write_file(LPCWSTR file_name, const void* buffer,
                                     DWORD bytes_to_write, LPDWORD bytes_written,
                                     LONGLONG offset, PDOKAN_FILE_INFO info) {
    if (read_only_) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    __try {
        const NodeKind k = kind(info);
        if (k == NodeKind::kAuxFile) {
            return aux_.write(file_name, buffer, bytes_to_write, bytes_written, offset,
                              info->WriteToEndOfFile);
        }
        if (k != NodeKind::kDiskImage) {
            return STATUS_INVALID_PARAMETER;
        }
        VirtualDiskEntry* entry = find_disk(file_name);
        if (!entry || !entry->disk) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        return entry->disk->write(static_cast<std::uint64_t>(offset), buffer,
                                  bytes_to_write, bytes_written);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (bytes_written) {
            *bytes_written = 0;
        }
        return STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS DokanFileSystem::flush_file_buffers(LPCWSTR file_name,
                                             PDOKAN_FILE_INFO info) {
    if (kind(info) == NodeKind::kDiskImage) {
        if (VirtualDiskEntry* entry = find_disk(file_name); entry && entry->backing) {
            entry->backing->flush();
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS DokanFileSystem::get_file_information(LPCWSTR file_name,
                                               LPBY_HANDLE_FILE_INFORMATION buffer,
                                               PDOKAN_FILE_INFO info) {
    ZeroMemory(buffer, sizeof(BY_HANDLE_FILE_INFORMATION));
    const NodeKind k = kind(info);

    if (k == NodeKind::kRootDir) {
        buffer->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        return STATUS_SUCCESS;
    }

    if (k == NodeKind::kDiskImage) {
        VirtualDiskEntry* entry = find_disk(file_name);
        if (!entry || !entry->disk || !entry->backing) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        const std::uint64_t file_size = entry->disk->file_size();
        buffer->dwFileAttributes =
            FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
            FILE_ATTRIBUTE_READONLY;
        buffer->nFileSizeLow = static_cast<DWORD>(file_size & 0xFFFFFFFF);
        buffer->nFileSizeHigh = static_cast<DWORD>(file_size >> 32);
        buffer->nNumberOfLinks = 1;
        entry->backing->get_times(&buffer->ftCreationTime, &buffer->ftLastAccessTime,
                                  &buffer->ftLastWriteTime);
        return STATUS_SUCCESS;
    }

    if (k == NodeKind::kAuxFile || k == NodeKind::kAuxDir) {
        AuxEntryInfo e;
        if (!aux_.get_info(file_name, &e)) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        buffer->dwFileAttributes =
            e.is_directory
                ? FILE_ATTRIBUTE_DIRECTORY
                : (FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
        buffer->nFileSizeLow = static_cast<DWORD>(e.size & 0xFFFFFFFF);
        buffer->nFileSizeHigh = static_cast<DWORD>(e.size >> 32);
        buffer->nNumberOfLinks = 1;
        buffer->ftCreationTime = e.creation;
        buffer->ftLastAccessTime = e.last_access;
        buffer->ftLastWriteTime = e.last_write;
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

NTSTATUS DokanFileSystem::find_files(LPCWSTR file_name, PFillFindData fill_find_data,
                                     PDOKAN_FILE_INFO info) {
    const NodeKind k = kind(info);
    if (k != NodeKind::kRootDir && k != NodeKind::kAuxDir) {
        return STATUS_INVALID_PARAMETER;
    }

    fill_dot_entries(fill_find_data, info);

    if (k == NodeKind::kRootDir) {
        for (auto& entry : disks_) {
            if (!entry.disk || !entry.backing) {
                continue;
            }
            FILETIME c{}, a{}, w{};
            entry.backing->get_times(&c, &a, &w);
            WIN32_FIND_DATAW find_data{};
            find_data.dwFileAttributes =
                FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                FILE_ATTRIBUTE_READONLY;
            const std::uint64_t file_size = entry.disk->file_size();
            find_data.nFileSizeLow = static_cast<DWORD>(file_size & 0xFFFFFFFF);
            find_data.nFileSizeHigh = static_cast<DWORD>(file_size >> 32);
            find_data.ftCreationTime = c;
            find_data.ftLastAccessTime = a;
            find_data.ftLastWriteTime = w;
            wcscpy_s(find_data.cFileName, entry.leaf_name.c_str());
            fill_find_data(&find_data, info);
        }
    }

    aux_.enumerate_children(file_name, [&](const AuxEntryInfo& e) {
        WIN32_FIND_DATAW fd{};
        fd.dwFileAttributes =
            e.is_directory
                ? FILE_ATTRIBUTE_DIRECTORY
                : (FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
        fd.nFileSizeLow = static_cast<DWORD>(e.size & 0xFFFFFFFF);
        fd.nFileSizeHigh = static_cast<DWORD>(e.size >> 32);
        fd.ftCreationTime = e.creation;
        fd.ftLastAccessTime = e.last_access;
        fd.ftLastWriteTime = e.last_write;
        wcscpy_s(fd.cFileName, e.display_name.c_str());
        fill_find_data(&fd, info);
    });

    return STATUS_SUCCESS;
}

NTSTATUS DokanFileSystem::set_file_attributes(LPCWSTR file_name,
                                              DWORD file_attributes,
                                              PDOKAN_FILE_INFO info) {
    if (read_only_) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    if (kind(info) == NodeKind::kDiskImage) {
        if (VirtualDiskEntry* entry = find_disk(file_name); entry && entry->backing) {
            entry->backing->set_overlay_attributes(file_attributes);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS DokanFileSystem::set_file_time(LPCWSTR file_name, const FILETIME* creation,
                                        const FILETIME* last_access,
                                        const FILETIME* last_write,
                                        PDOKAN_FILE_INFO info) {
    if (read_only_) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    const NodeKind k = kind(info);
    if (k == NodeKind::kAuxFile || k == NodeKind::kAuxDir) {
        aux_.set_times(file_name, creation, last_access, last_write);
        return STATUS_SUCCESS;
    }
    if (k == NodeKind::kDiskImage) {
        if (VirtualDiskEntry* entry = find_disk(file_name); entry && entry->backing) {
            entry->backing->set_overlay_times(creation, last_access, last_write);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS DokanFileSystem::delete_file(LPCWSTR file_name, PDOKAN_FILE_INFO info) {
    if (read_only_) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    if (kind(info) == NodeKind::kAuxFile) {
        return aux_.remove_file(file_name);
    }
    return STATUS_ACCESS_DENIED;
}

NTSTATUS DokanFileSystem::delete_directory(LPCWSTR file_name, PDOKAN_FILE_INFO info) {
    if (read_only_) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    if (kind(info) == NodeKind::kAuxDir) {
        return aux_.remove_directory(file_name);
    }
    return STATUS_ACCESS_DENIED;
}

NTSTATUS DokanFileSystem::move_file(LPCWSTR file_name, LPCWSTR new_file_name,
                                    BOOL replace_if_existing, PDOKAN_FILE_INFO info) {
    if (read_only_) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    const NodeKind k = kind(info);
    if (k == NodeKind::kAuxFile || k == NodeKind::kAuxDir) {
        return aux_.move(file_name, new_file_name, replace_if_existing);
    }
    return STATUS_ACCESS_DENIED;
}

NTSTATUS DokanFileSystem::set_end_of_file(LPCWSTR file_name, LONGLONG byte_offset,
                                          PDOKAN_FILE_INFO info) {
    if (read_only_) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    const NodeKind k = kind(info);
    if (k == NodeKind::kAuxFile) {
        return aux_.set_end_of_file(file_name, byte_offset);
    }
    if (k != NodeKind::kDiskImage) {
        return STATUS_INVALID_PARAMETER;
    }
    if (byte_offset < 0) {
        return STATUS_INVALID_PARAMETER;
    }
    VirtualDiskEntry* entry = find_disk(file_name);
    if (!entry || !entry->disk) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    return entry->disk->set_end_of_file(static_cast<std::uint64_t>(byte_offset));
}

NTSTATUS DokanFileSystem::set_allocation_size(LPCWSTR file_name, LONGLONG alloc_size,
                                              PDOKAN_FILE_INFO info) {
    return set_end_of_file(file_name, alloc_size, info);
}

int DokanFileSystem::mount(const std::wstring& mount_point) {
    if (instance_ != nullptr) {
        return DOKAN_ERROR;
    }

    mount_point_ = mount_point;
    actual_mount_point_.clear();

    ZeroMemory(&options_, sizeof(options_));
    options_.Version = DOKAN_VERSION;
    options_.SingleThread = FALSE;
    options_.Options = DOKAN_OPTION_MOUNT_MANAGER;
    if (read_only_) {
        options_.Options |= DOKAN_OPTION_WRITE_PROTECT;
    }
    options_.MountPoint = mount_point_.c_str();
    options_.Timeout = 15000;
    options_.AllocationUnitSize = 512;
    options_.SectorSize = 512;
    options_.GlobalContext = reinterpret_cast<ULONG64>(this);

    ZeroMemory(&ops_, sizeof(ops_));
    ops_.ZwCreateFile = s_create_file;
    ops_.Cleanup = s_cleanup;
    ops_.CloseFile = s_close_file;
    ops_.ReadFile = s_read_file;
    ops_.WriteFile = s_write_file;
    ops_.FlushFileBuffers = s_flush_file_buffers;
    ops_.GetFileInformation = s_get_file_information;
    ops_.FindFiles = s_find_files;
    ops_.SetFileAttributes = s_set_file_attributes;
    ops_.SetFileTime = s_set_file_time;
    ops_.DeleteFile = s_delete_file;
    ops_.DeleteDirectory = s_delete_directory;
    ops_.MoveFile = s_move_file;
    ops_.SetEndOfFile = s_set_end_of_file;
    ops_.SetAllocationSize = s_set_allocation_size;
    ops_.GetVolumeInformation = s_get_volume_information;
    ops_.GetDiskFreeSpace = s_get_disk_free_space;
    ops_.GetFileSecurity = s_get_file_security;
    ops_.SetFileSecurity = s_set_file_security;
    ops_.Mounted = s_mounted;
    ops_.Unmounted = s_unmounted;

    acquire_dokan_library();

    DOKAN_HANDLE instance = nullptr;
    const int status = DokanCreateFileSystem(&options_, &ops_, &instance);
    if (status != DOKAN_SUCCESS) {
        release_dokan_library();
        instance_ = nullptr;
        return status;
    }

    instance_ = instance;
    return DOKAN_SUCCESS;
}

void DokanFileSystem::close(DWORD close_timeout_ms) {
    if (instance_ == nullptr) {
        return;
    }

    DOKAN_HANDLE inst = instance_;
    instance_ = nullptr;

    const std::wstring mount_point =
        actual_mount_point_.empty() ? mount_point_ : actual_mount_point_;

    bool removed = false;
    constexpr int kMaxRetries = 5;
    for (int i = 0; i < kMaxRetries; ++i) {
        Sleep(1000);
        if (request_dokan_unmount(mount_point)) {
            removed = true;
            break;
        }
    }

    const DWORD wait_ms =
        removed ? close_timeout_ms : (std::min)(close_timeout_ms, static_cast<DWORD>(3000));
    const DWORD waited = DokanWaitForFileSystemClosed(inst, wait_ms);

    std::atomic<bool> closed{false};
    std::thread closer([&]() {
        DokanCloseHandle(inst);
        closed.store(true, std::memory_order_release);
    });

    const DWORD handle_timeout = (waited == WAIT_TIMEOUT) ? 2000 : 8000;
    constexpr DWORD kStep = 50;
    DWORD elapsed = 0;
    while (!closed.load(std::memory_order_acquire) && elapsed < handle_timeout) {
        Sleep(kStep);
        elapsed += kStep;
    }

    bool abandoned = false;
    if (closed.load(std::memory_order_acquire)) {
        closer.join();
    } else {
        closer.detach();
        abandoned = true;
    }

    if (!abandoned) {
        for (auto& entry : disks_) {
            if (entry.backing) {
                entry.backing->flush();
            }
        }
        release_dokan_library();
    }
}

bool DokanFileSystem::is_running() const {
    return instance_ != nullptr && DokanIsFileSystemRunning(instance_);
}

DWORD DokanFileSystem::wait_closed(DWORD milliseconds) const {
    if (instance_ == nullptr) {
        return WAIT_OBJECT_0;
    }
    return DokanWaitForFileSystemClosed(instance_, milliseconds);
}

} // namespace aegra::adapters::dokan::detail
