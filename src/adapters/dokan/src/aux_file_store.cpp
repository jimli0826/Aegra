#include "aux_file_store.h"

#include <cwctype>

#include <cstring>

namespace aegra::adapters::dokan::detail {
namespace {

constexpr ULONG kNtFileSupersede = 0;
constexpr ULONG kNtFileOpen = 1;
constexpr ULONG kNtFileCreate = 2;
constexpr ULONG kNtFileOpenIf = 3;
constexpr ULONG kNtFileOverwrite = 4;
constexpr ULONG kNtFileOverwriteIf = 5;

constexpr ULONG kNtFileDirectoryFile = 0x00000001;
constexpr ULONG kNtFileNonDirectoryFile = 0x00000040;

} // namespace

std::wstring AuxFileStore::key(LPCWSTR path) {
    std::wstring k(path);
    for (auto& c : k) {
        c = towlower(c);
    }
    return k;
}

std::wstring AuxFileStore::leaf_name(LPCWSTR path) {
    std::wstring p(path);
    const std::size_t pos = p.find_last_of(L'\\');
    return (pos == std::wstring::npos) ? p : p.substr(pos + 1);
}

std::wstring AuxFileStore::parent_key(const std::wstring& key) {
    const std::size_t pos = key.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos == 0) {
        return L"\\";
    }
    return key.substr(0, pos);
}

bool AuxFileStore::parent_exists(const std::wstring& key) {
    const std::wstring parent = parent_key(key);
    if (parent == L"\\") {
        return true;
    }
    const auto it = files_.find(parent);
    return it != files_.end() && it->second.is_directory;
}

NTSTATUS AuxFileStore::create(LPCWSTR file_name, ULONG create_disposition,
                              ULONG create_options, bool* out_is_directory) {
    std::lock_guard lock(mutex_);
    const std::wstring k = key(file_name);
    auto it = files_.find(k);
    const bool exists = (it != files_.end());
    const bool want_directory = (create_options & kNtFileDirectoryFile) != 0;

    switch (create_disposition) {
    case kNtFileCreate:
        if (exists) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
        break;
    case kNtFileOpen:
    case kNtFileOverwrite:
        if (!exists) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        break;
    case kNtFileSupersede:
    case kNtFileOpenIf:
    case kNtFileOverwriteIf:
    default:
        break;
    }

    if (exists) {
        if (it->second.is_directory &&
            (create_options & kNtFileNonDirectoryFile)) {
            return STATUS_FILE_IS_A_DIRECTORY;
        }
        if (!it->second.is_directory && want_directory) {
            return STATUS_NOT_A_DIRECTORY;
        }
    }

    if (!exists) {
        if (!parent_exists(k)) {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        MemEntry entry;
        entry.is_directory = want_directory;
        entry.display_name = leaf_name(file_name);
        GetSystemTimeAsFileTime(&entry.creation);
        entry.last_access = entry.creation;
        entry.last_write = entry.creation;
        it = files_.emplace(k, std::move(entry)).first;
    } else if (!it->second.is_directory &&
               (create_disposition == kNtFileSupersede ||
                create_disposition == kNtFileOverwrite ||
                create_disposition == kNtFileOverwriteIf)) {
        it->second.data.clear();
    }

    if (out_is_directory) {
        *out_is_directory = it->second.is_directory;
    }
    return STATUS_SUCCESS;
}

void AuxFileStore::cleanup(LPCWSTR file_name, bool delete_pending) {
    if (!delete_pending) {
        return;
    }
    std::lock_guard lock(mutex_);
    files_.erase(key(file_name));
}

NTSTATUS AuxFileStore::read(LPCWSTR file_name, void* buffer, DWORD buffer_len,
                            LPDWORD bytes_read, LONGLONG offset) {
    std::lock_guard lock(mutex_);
    const auto it = files_.find(key(file_name));
    if (it == files_.end() || it->second.is_directory) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    MemEntry& e = it->second;
    const std::uint64_t off = static_cast<std::uint64_t>(offset);
    if (off >= e.data.size()) {
        *bytes_read = 0;
        return STATUS_SUCCESS;
    }
    const DWORD avail = static_cast<DWORD>(e.data.size() - off);
    const DWORD n = (buffer_len < avail) ? buffer_len : avail;
    std::memcpy(buffer, e.data.data() + off, n);
    *bytes_read = n;
    return STATUS_SUCCESS;
}

NTSTATUS AuxFileStore::write(LPCWSTR file_name, const void* buffer,
                             DWORD bytes_to_write, LPDWORD bytes_written,
                             LONGLONG offset, BOOL write_to_end) {
    if (!write_to_end && offset < 0) {
        return STATUS_INVALID_PARAMETER;
    }

    std::lock_guard lock(mutex_);
    const auto it = files_.find(key(file_name));
    if (it == files_.end() || it->second.is_directory) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    MemEntry& e = it->second;
    const std::uint64_t off =
        write_to_end ? e.data.size() : static_cast<std::uint64_t>(offset);
    if (off + bytes_to_write > e.data.size()) {
        e.data.resize(static_cast<std::size_t>(off + bytes_to_write), 0);
    }
    std::memcpy(e.data.data() + off, buffer, bytes_to_write);
    GetSystemTimeAsFileTime(&e.last_write);
    e.last_access = e.last_write;
    *bytes_written = bytes_to_write;
    return STATUS_SUCCESS;
}

bool AuxFileStore::get_info(LPCWSTR file_name, AuxEntryInfo* out) {
    std::lock_guard lock(mutex_);
    const auto it = files_.find(key(file_name));
    if (it == files_.end()) {
        return false;
    }
    const MemEntry& e = it->second;
    out->is_directory = e.is_directory;
    out->size = e.data.size();
    out->creation = e.creation;
    out->last_access = e.last_access;
    out->last_write = e.last_write;
    out->display_name = e.display_name;
    return true;
}

NTSTATUS AuxFileStore::set_end_of_file(LPCWSTR file_name, LONGLONG byte_offset) {
    if (byte_offset < 0) {
        return STATUS_INVALID_PARAMETER;
    }
    std::lock_guard lock(mutex_);
    const auto it = files_.find(key(file_name));
    if (it == files_.end() || it->second.is_directory) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    it->second.data.resize(static_cast<std::size_t>(byte_offset), 0);
    return STATUS_SUCCESS;
}

void AuxFileStore::set_times(LPCWSTR file_name, const FILETIME* creation,
                             const FILETIME* last_access,
                             const FILETIME* last_write) {
    std::lock_guard lock(mutex_);
    const auto it = files_.find(key(file_name));
    if (it == files_.end()) {
        return;
    }
    if (creation && creation->dwLowDateTime) {
        it->second.creation = *creation;
    }
    if (last_access && last_access->dwLowDateTime) {
        it->second.last_access = *last_access;
    }
    if (last_write && last_write->dwLowDateTime) {
        it->second.last_write = *last_write;
    }
}

NTSTATUS AuxFileStore::remove_file(LPCWSTR file_name) {
    std::lock_guard lock(mutex_);
    return (files_.find(key(file_name)) != files_.end())
               ? STATUS_SUCCESS
               : STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS AuxFileStore::remove_directory(LPCWSTR file_name) {
    std::lock_guard lock(mutex_);
    const std::wstring dir_key = key(file_name);
    for (const auto& kv : files_) {
        if (parent_key(kv.first) == dir_key) {
            return STATUS_DIRECTORY_NOT_EMPTY;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS AuxFileStore::move(LPCWSTR file_name, LPCWSTR new_file_name,
                            BOOL replace_if_existing) {
    std::lock_guard lock(mutex_);
    const std::wstring src_key = key(file_name);
    const std::wstring dst_key = key(new_file_name);
    auto src = files_.find(src_key);
    if (src == files_.end()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    auto dst = files_.find(dst_key);
    if (dst != files_.end()) {
        if (!replace_if_existing) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
        files_.erase(dst);
    }
    if (!parent_exists(dst_key)) {
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    MemEntry moved = std::move(src->second);
    moved.display_name = leaf_name(new_file_name);
    files_.erase(src);
    files_.emplace(dst_key, std::move(moved));
    return STATUS_SUCCESS;
}

void AuxFileStore::enumerate_children(
    LPCWSTR dir_path, const std::function<void(const AuxEntryInfo&)>& fn) {
    std::wstring dir_key = key(dir_path);
    if (dir_key.empty()) {
        dir_key = L"\\";
    }
    std::lock_guard lock(mutex_);
    for (const auto& kv : files_) {
        if (parent_key(kv.first) != dir_key) {
            continue;
        }
        const MemEntry& e = kv.second;
        AuxEntryInfo info;
        info.is_directory = e.is_directory;
        info.size = e.data.size();
        info.creation = e.creation;
        info.last_access = e.last_access;
        info.last_write = e.last_write;
        info.display_name = e.display_name;
        fn(info);
    }
}

} // namespace aegra::adapters::dokan::detail
