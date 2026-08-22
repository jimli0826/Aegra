#include "file_set_fs.h"

#include "dokan_file_system.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <cwctype>
#include <optional>
#include <span>
#include <thread>

namespace aegra::adapters::dokan::detail {
namespace {

constexpr std::uint32_t kListPageSize = 512;
constexpr ULONGLONG kFallbackTotalBytes = 1024ULL * 1024 * 1024;

// Access bits that would mutate data or namespace; attribute-only writes are
// tolerated at open and rejected in the Set* callbacks instead, so Explorer
// copy flows do not fail up front.
constexpr ACCESS_MASK kWriteAccessMask =
    FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | GENERIC_WRITE | GENERIC_ALL;

FILETIME to_filetime(const std::uint64_t value) {
    FILETIME out{};
    out.dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFULL);
    out.dwHighDateTime = static_cast<DWORD>(value >> 32U);
    return out;
}

std::wstring to_lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

// UTF-16LE Index name -> leaf component. Separators and controls never form a
// valid leaf; replace them so one entry cannot alias another path.
std::wstring decode_entry_name(const contracts::EncodedName& name) {
    if (name.encoding != contracts::NameEncoding::kWindowsUtf16Le ||
        name.bytes.empty() || (name.bytes.size() % 2) != 0) {
        return {};
    }
    std::wstring out(name.bytes.size() / 2, L'\0');
    memcpy(out.data(), name.bytes.data(), name.bytes.size());
    for (auto& c : out) {
        if (c == L'\\' || c == L'/' || c == L':' || c < 0x20) {
            c = L'_';
        }
    }
    if (out == L"." || out == L"..") {
        return {};
    }
    return out;
}

NTSTATUS status_from_error(const base::Error& error) {
    switch (error.code) {
    case base::ErrorCode::kNotFound:
        return STATUS_OBJECT_NAME_NOT_FOUND;
    case base::ErrorCode::kCancelled:
        return STATUS_CANCELLED;
    case base::ErrorCode::kInvalidArgument:
        return STATUS_INVALID_PARAMETER;
    default:
        return STATUS_IO_DEVICE_ERROR;
    }
}

DWORD entry_attributes(const contracts::FileEntryDesc& desc) {
    DWORD attributes = desc.attributes;
    if (desc.kind == contracts::FileEntryKind::kDirectory) {
        attributes |= FILE_ATTRIBUTE_DIRECTORY;
    } else {
        attributes &= ~static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY);
    }
    attributes &= ~static_cast<DWORD>(FILE_ATTRIBUTE_NORMAL);
    attributes |= FILE_ATTRIBUTE_READONLY;
    return attributes;
}

const contracts::FileStreamDesc* find_main_stream(const contracts::FileEntryDesc& desc) {
    for (const auto& stream : desc.streams) {
        if (stream.stream_kind == contracts::FileStreamKind::kMain &&
            stream.name.bytes.empty()) {
            return &stream;
        }
    }
    return nullptr;
}

} // namespace

FileSetFileSystem::FileSetFileSystem(ports::IFileRecoveryPointReader& reader)
    : reader_(&reader) {
    auto root = std::make_unique<Node>();
    root->desc.entry_id = 0;
    root->desc.kind = contracts::FileEntryKind::kDirectory;
    root->desc.attributes = FILE_ATTRIBUTE_DIRECTORY;
    nodes_[0] = std::move(root);
}

FileSetFileSystem::~FileSetFileSystem() { close(); }

FileSetFileSystem* FileSetFileSystem::self(PDOKAN_FILE_INFO info) {
    return reinterpret_cast<FileSetFileSystem*>(info->DokanOptions->GlobalContext);
}

FileSetFileSystem::Node* FileSetFileSystem::node_for_locked(const std::uint64_t entry_id) {
    const auto found = nodes_.find(entry_id);
    return found == nodes_.end() ? nullptr : found->second.get();
}

NTSTATUS FileSetFileSystem::ensure_children_locked(Node& directory) {
    if (directory.children_loaded) {
        return STATUS_SUCCESS;
    }
    if (directory.desc.kind != contracts::FileEntryKind::kDirectory) {
        return STATUS_NOT_A_DIRECTORY;
    }

    std::optional<std::string> token;
    do {
        auto page = reader_->list_children(directory.desc.entry_id, kListPageSize, token, {});
        if (!page) {
            return status_from_error(page.error());
        }
        for (const auto& item : page.value().items) {
            std::uint64_t child_id = 0;
            const auto& id_text = item.entry_id;
            const auto parsed =
                std::from_chars(id_text.data(), id_text.data() + id_text.size(), child_id);
            if (parsed.ec != std::errc{} || child_id == 0) {
                continue;
            }
            auto described = reader_->describe_entry(child_id, {});
            if (!described) {
                return status_from_error(described.error());
            }
            auto node = std::make_unique<Node>();
            node->desc = std::move(described).value();
            node->name = decode_entry_name(node->desc.name);
            if (node->name.empty()) {
                node->name = L"entry_" + std::to_wstring(child_id);
            }
            auto lower = to_lower(node->name);
            if (directory.children_by_lower_name.contains(lower)) {
                node->name += L"~" + std::to_wstring(child_id);
                lower = to_lower(node->name);
            }
            directory.children_by_lower_name.emplace(std::move(lower), child_id);
            directory.child_ids.push_back(child_id);
            nodes_[child_id] = std::move(node);
        }
        token = std::move(page.value().continuation_token);
    } while (token.has_value());

    directory.children_loaded = true;
    return STATUS_SUCCESS;
}

NTSTATUS FileSetFileSystem::resolve_path_locked(LPCWSTR path, Node** out) {
    *out = nullptr;
    if (path == nullptr || path[0] != L'\\') {
        return STATUS_INVALID_PARAMETER;
    }

    Node* current = node_for_locked(0);
    const wchar_t* cursor = path + 1;
    while (*cursor != L'\0') {
        const wchar_t* end = cursor;
        while (*end != L'\0' && *end != L'\\') {
            ++end;
        }
        if (end != cursor) {
            const auto status = ensure_children_locked(*current);
            if (status != STATUS_SUCCESS) {
                return status;
            }
            const auto lower = to_lower(std::wstring(cursor, end));
            const auto found = current->children_by_lower_name.find(lower);
            if (found == current->children_by_lower_name.end()) {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            current = node_for_locked(found->second);
            if (current == nullptr) {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
        }
        cursor = (*end == L'\\') ? end + 1 : end;
    }
    *out = current;
    return STATUS_SUCCESS;
}

NTSTATUS FileSetFileSystem::create_file(LPCWSTR file_name, const ACCESS_MASK desired_access,
                                        const ULONG create_disposition,
                                        const ULONG create_options, PDOKAN_FILE_INFO info) {
    std::lock_guard lock(mutex_);
    Node* node = nullptr;
    const auto status = resolve_path_locked(file_name, &node);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
        const bool wants_create = create_disposition == FILE_CREATE ||
                                  create_disposition == FILE_OPEN_IF ||
                                  create_disposition == FILE_OVERWRITE_IF ||
                                  create_disposition == FILE_SUPERSEDE;
        return wants_create ? STATUS_MEDIA_WRITE_PROTECTED : STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (status != STATUS_SUCCESS) {
        return status;
    }

    if (create_disposition == FILE_CREATE) {
        return STATUS_OBJECT_NAME_COLLISION;
    }
    if (create_disposition == FILE_SUPERSEDE || create_disposition == FILE_OVERWRITE ||
        create_disposition == FILE_OVERWRITE_IF) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    if ((desired_access & kWriteAccessMask) != 0) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    const bool is_directory = node->desc.kind == contracts::FileEntryKind::kDirectory;
    if (is_directory && (create_options & FILE_NON_DIRECTORY_FILE) != 0) {
        return STATUS_FILE_IS_A_DIRECTORY;
    }
    if (!is_directory && (create_options & FILE_DIRECTORY_FILE) != 0) {
        return STATUS_NOT_A_DIRECTORY;
    }

    info->IsDirectory = is_directory ? TRUE : FALSE;
    info->Context = node->desc.entry_id + 1; // 0 keeps meaning "unset"
    return STATUS_SUCCESS;
}

NTSTATUS FileSetFileSystem::read_file(LPCWSTR file_name, void* buffer, const DWORD buffer_len,
                                      LPDWORD read_length, const LONGLONG offset,
                                      PDOKAN_FILE_INFO info) {
    *read_length = 0;
    if (offset < 0) {
        return STATUS_INVALID_PARAMETER;
    }

    std::lock_guard lock(mutex_);
    Node* node = nullptr;
    if (info->Context != 0) {
        node = node_for_locked(info->Context - 1);
    }
    if (node == nullptr) {
        const auto status = resolve_path_locked(file_name, &node);
        if (status != STATUS_SUCCESS) {
            return status;
        }
    }
    if (node->desc.kind == contracts::FileEntryKind::kDirectory) {
        return STATUS_ACCESS_DENIED;
    }

    const auto size = node->desc.logical_size;
    const auto from = static_cast<std::uint64_t>(offset);
    if (from >= size || buffer_len == 0) {
        return STATUS_SUCCESS;
    }
    const auto* stream = find_main_stream(node->desc);
    if (stream == nullptr) {
        return STATUS_SUCCESS; // zero-length or metadata-only entry
    }

    const auto want = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer_len, size - from));
    std::size_t filled = 0;
    while (filled < want) {
        ports::FileStreamReadRequest request;
        request.stream_index = stream->stream_index;
        request.offset = from + filled;
        request.size = want - filled;
        auto read = reader_->read_stream(
            request,
            std::span<std::byte>(static_cast<std::byte*>(buffer) + filled, want - filled),
            {});
        if (!read) {
            return status_from_error(read.error());
        }
        if (read.value() == 0) {
            break; // short read at EOF
        }
        filled += read.value();
    }
    *read_length = static_cast<DWORD>(filled);
    return STATUS_SUCCESS;
}

NTSTATUS FileSetFileSystem::get_file_information(LPCWSTR file_name,
                                                 LPBY_HANDLE_FILE_INFORMATION buffer,
                                                 PDOKAN_FILE_INFO info) {
    std::lock_guard lock(mutex_);
    Node* node = nullptr;
    if (info->Context != 0) {
        node = node_for_locked(info->Context - 1);
    }
    if (node == nullptr) {
        const auto status = resolve_path_locked(file_name, &node);
        if (status != STATUS_SUCCESS) {
            return status;
        }
    }

    ZeroMemory(buffer, sizeof(*buffer));
    buffer->dwFileAttributes = entry_attributes(node->desc);
    buffer->ftCreationTime = to_filetime(node->desc.creation_time);
    buffer->ftLastAccessTime = to_filetime(node->desc.access_time);
    buffer->ftLastWriteTime = to_filetime(node->desc.write_time);
    buffer->dwVolumeSerialNumber = 0x41454752; // "AEGR"
    buffer->nFileSizeHigh = static_cast<DWORD>(node->desc.logical_size >> 32U);
    buffer->nFileSizeLow = static_cast<DWORD>(node->desc.logical_size & 0xFFFFFFFFULL);
    buffer->nNumberOfLinks = 1;
    buffer->nFileIndexHigh = static_cast<DWORD>(node->desc.entry_id >> 32U);
    buffer->nFileIndexLow = static_cast<DWORD>(node->desc.entry_id & 0xFFFFFFFFULL);
    return STATUS_SUCCESS;
}

NTSTATUS FileSetFileSystem::find_files(LPCWSTR file_name, PFillFindData fill_find_data,
                                       PDOKAN_FILE_INFO info) {
    std::lock_guard lock(mutex_);
    Node* directory = nullptr;
    const auto status = resolve_path_locked(file_name, &directory);
    if (status != STATUS_SUCCESS) {
        return status;
    }
    if (directory->desc.kind != contracts::FileEntryKind::kDirectory) {
        return STATUS_NOT_A_DIRECTORY;
    }
    const auto loaded = ensure_children_locked(*directory);
    if (loaded != STATUS_SUCCESS) {
        return loaded;
    }

    fill_dot_entries(fill_find_data, info);
    for (const auto child_id : directory->child_ids) {
        const Node* child = node_for_locked(child_id);
        if (child == nullptr) {
            continue;
        }
        WIN32_FIND_DATAW find_data{};
        find_data.dwFileAttributes = entry_attributes(child->desc);
        find_data.ftCreationTime = to_filetime(child->desc.creation_time);
        find_data.ftLastAccessTime = to_filetime(child->desc.access_time);
        find_data.ftLastWriteTime = to_filetime(child->desc.write_time);
        find_data.nFileSizeHigh = static_cast<DWORD>(child->desc.logical_size >> 32U);
        find_data.nFileSizeLow =
            static_cast<DWORD>(child->desc.logical_size & 0xFFFFFFFFULL);
        wcsncpy_s(find_data.cFileName, child->name.c_str(), _TRUNCATE);
        fill_find_data(&find_data, info);
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_create_file(
    LPCWSTR file_name, PDOKAN_IO_SECURITY_CONTEXT security_context,
    ACCESS_MASK desired_access, ULONG file_attributes, ULONG share_access,
    ULONG create_disposition, ULONG create_options, PDOKAN_FILE_INFO info) {
    UNREFERENCED_PARAMETER(security_context);
    UNREFERENCED_PARAMETER(file_attributes);
    UNREFERENCED_PARAMETER(share_access);
    return self(info)->create_file(file_name, desired_access, create_disposition,
                                   create_options, info);
}

void DOKAN_CALLBACK FileSetFileSystem::s_cleanup(LPCWSTR, PDOKAN_FILE_INFO) {}

void DOKAN_CALLBACK FileSetFileSystem::s_close_file(LPCWSTR, PDOKAN_FILE_INFO info) {
    info->Context = 0;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_read_file(LPCWSTR file_name, LPVOID buffer,
                                                       DWORD buffer_len,
                                                       LPDWORD read_length,
                                                       LONGLONG offset,
                                                       PDOKAN_FILE_INFO info) {
    return self(info)->read_file(file_name, buffer, buffer_len, read_length, offset,
                                 info);
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_write_file(LPCWSTR, LPCVOID, DWORD,
                                                        LPDWORD bytes_written, LONGLONG,
                                                        PDOKAN_FILE_INFO) {
    *bytes_written = 0;
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_flush_file_buffers(LPCWSTR,
                                                                PDOKAN_FILE_INFO) {
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_get_file_information(
    LPCWSTR file_name, LPBY_HANDLE_FILE_INFORMATION buffer, PDOKAN_FILE_INFO info) {
    return self(info)->get_file_information(file_name, buffer, info);
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_find_files(LPCWSTR file_name,
                                                        PFillFindData fill_find_data,
                                                        PDOKAN_FILE_INFO info) {
    return self(info)->find_files(file_name, fill_find_data, info);
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_set_file_attributes(LPCWSTR, DWORD,
                                                                 PDOKAN_FILE_INFO) {
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_set_file_time(LPCWSTR, CONST FILETIME*,
                                                           CONST FILETIME*,
                                                           CONST FILETIME*,
                                                           PDOKAN_FILE_INFO) {
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_delete_file(LPCWSTR, PDOKAN_FILE_INFO) {
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_delete_directory(LPCWSTR,
                                                              PDOKAN_FILE_INFO) {
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_move_file(LPCWSTR, LPCWSTR, BOOL,
                                                       PDOKAN_FILE_INFO) {
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_set_end_of_file(LPCWSTR, LONGLONG,
                                                             PDOKAN_FILE_INFO) {
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_set_allocation_size(LPCWSTR, LONGLONG,
                                                                 PDOKAN_FILE_INFO) {
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_get_volume_information(
    LPWSTR volume_name_buffer, DWORD volume_name_size, LPDWORD volume_serial_number,
    LPDWORD maximum_component_length, LPDWORD file_system_flags,
    LPWSTR file_system_name_buffer, DWORD file_system_name_size, PDOKAN_FILE_INFO info) {
    UNREFERENCED_PARAMETER(info);
    wcscpy_s(volume_name_buffer, volume_name_size, L"Aegra Files");
    *volume_serial_number = 0x41454752; // "AEGR"
    *maximum_component_length = 255;
    *file_system_flags =
        FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK | FILE_READ_ONLY_VOLUME;
    if (file_system_name_size > 0) {
        wcscpy_s(file_system_name_buffer, file_system_name_size, L"NTFS");
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_get_disk_free_space(
    PULONGLONG free_bytes_available, PULONGLONG total_number_of_bytes,
    PULONGLONG total_number_of_free_bytes, PDOKAN_FILE_INFO info) {
    UNREFERENCED_PARAMETER(info);
    // The archive index has no cheap total-bytes figure; report a placeholder.
    *total_number_of_bytes = kFallbackTotalBytes;
    *total_number_of_free_bytes = 0;
    *free_bytes_available = 0;
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_get_file_security(LPCWSTR,
                                                               PSECURITY_INFORMATION,
                                                               PSECURITY_DESCRIPTOR,
                                                               ULONG, PULONG,
                                                               PDOKAN_FILE_INFO) {
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_set_file_security(LPCWSTR,
                                                               PSECURITY_INFORMATION,
                                                               PSECURITY_DESCRIPTOR,
                                                               ULONG, PDOKAN_FILE_INFO) {
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_mounted(LPCWSTR mount_point,
                                                     PDOKAN_FILE_INFO info) {
    if (info && mount_point) {
        if (FileSetFileSystem* fs = self(info)) {
            fs->actual_mount_point_ = mount_point;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK FileSetFileSystem::s_unmounted(PDOKAN_FILE_INFO) {
    return STATUS_SUCCESS;
}

int FileSetFileSystem::mount(const std::wstring& mount_point) {
    if (instance_ != nullptr) {
        return DOKAN_ERROR;
    }

    mount_point_ = mount_point;
    actual_mount_point_.clear();

    ZeroMemory(&options_, sizeof(options_));
    options_.Version = DOKAN_VERSION;
    options_.SingleThread = FALSE;
    options_.Options = DOKAN_OPTION_MOUNT_MANAGER | DOKAN_OPTION_WRITE_PROTECT;
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

void FileSetFileSystem::close(DWORD close_timeout_ms) {
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

    if (closed.load(std::memory_order_acquire)) {
        closer.join();
        release_dokan_library();
    } else {
        closer.detach();
    }
}

bool FileSetFileSystem::is_running() const {
    return instance_ != nullptr && DokanIsFileSystemRunning(instance_);
}

} // namespace aegra::adapters::dokan::detail
