#include "pch.h"

#include "archive_shell_model.h"

#include "archive_chain_resolver.h"
#include "password_dialog.h"
#include "resource.h"
#include "shell_debug_log.h"
#include "shell_strings.h"

#include "aegra/adapters/ntfs/ntfs_reader.h"
#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/base/cancellation.h"
#include "aegra/base/error.h"
#include "aegra/contracts/file_set.h"
#include "aegra/format/manifest.h"
#include "aegra/ports/backup_session.h"
#include "aegra/ports/file_recovery_point.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace aegra::shell {
namespace {
namespace archive_api = aegra::adapters::personal_archive;
namespace ntfs_api = aegra::adapters::ntfs;
namespace fs = std::filesystem;
constexpr wchar_t kDiskPrefix[] = L"disk:";
constexpr wchar_t kVolumePrefix[] = L"vol:";
constexpr wchar_t kNtfsPrefix[] = L"ntfs:";
constexpr wchar_t kFileSetPrefix[] = L"fs:";
constexpr wchar_t kErrorItemId[] = L"error";
[[nodiscard]] std::wstring trim_trailing_slash(std::wstring path) {
    while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}
[[nodiscard]] std::wstring strip_extended_path_prefix(std::wstring path) {
    // GetFinalPathName/GetFullPathName may yield \\?\C:\... or \\?\UNC\server\share\...
    if (path.size() >= 8 && _wcsnicmp(path.c_str(), L"\\\\?\\UNC\\", 8) == 0) {
        path = L"\\\\" + path.substr(8);
    } else if (path.size() >= 4 && _wcsnicmp(path.c_str(), L"\\\\?\\", 4) == 0) {
        path = path.substr(4);
    }
    return path;
}
/// Stable session / logon-credential key so Explorer path variants share one open.
[[nodiscard]] std::wstring normalize_session_key(std::wstring path) {
    path = trim_trailing_slash(std::move(path));
    if (path.empty()) {
        return path;
    }
    const DWORD needed = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed > 1U) {
        std::wstring full(static_cast<std::size_t>(needed), L'\0');
        const DWORD written = ::GetFullPathNameW(path.c_str(), needed, full.data(), nullptr);
        if (written > 0U && written < needed) {
            full.resize(static_cast<std::size_t>(written));
            path = std::move(full);
        }
    }
    // Prefer the long path form so 8.3 and long names collapse to one key.
    const DWORD long_needed = ::GetLongPathNameW(path.c_str(), nullptr, 0);
    if (long_needed > 1U) {
        std::wstring long_path(static_cast<std::size_t>(long_needed), L'\0');
        const DWORD long_written = ::GetLongPathNameW(path.c_str(), long_path.data(), long_needed);
        if (long_written > 0U && long_written < long_needed) {
            long_path.resize(static_cast<std::size_t>(long_written));
            path = std::move(long_path);
        }
    }
    path = strip_extended_path_prefix(std::move(path));
    path = trim_trailing_slash(std::move(path));
    for (auto& ch : path) {
        if (ch == L'/') {
            ch = L'\\';
        }
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return path;
}

[[nodiscard]] std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int length =
        ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                          length);
    return result;
}

[[nodiscard]] std::wstring u16_to_wide(const std::u16string& value) {
    if (value.empty()) {
        return {};
    }
    return std::wstring(value.begin(), value.end());
}

[[nodiscard]] std::wstring encoded_name_to_wide(const contracts::EncodedName& name) {
    if (name.bytes.empty() || (name.bytes.size() % 2U) != 0U) {
        return L".";
    }
    const auto unit_count = name.bytes.size() / 2U;
    std::wstring wide(unit_count, L'\0');
    std::memcpy(wide.data(), name.bytes.data(), name.bytes.size());
    if (wide.empty()) {
        return L".";
    }
    return wide;
}

[[nodiscard]] std::string to_upper_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return value;
}

[[nodiscard]] bool is_ntfs_filesystem(const std::string& filesystem_utf8) {
    return to_upper_ascii(filesystem_utf8) == "NTFS";
}

[[nodiscard]] std::uint32_t folder_attributes() noexcept {
    return SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE | SFGAO_READONLY | SFGAO_CANCOPY;
}

[[nodiscard]] std::uint32_t file_attributes_shell() noexcept {
    return SFGAO_CANCOPY | SFGAO_STREAM | SFGAO_READONLY;
}

[[nodiscard]] std::uint32_t ntfs_shell_attributes(const ntfs_api::NtfsEntry& entry) noexcept {
    std::uint32_t attributes = entry.is_directory ? folder_attributes() : file_attributes_shell();
    if (entry.is_hidden || entry.is_system) {
        attributes |= SFGAO_HIDDEN;
    }
    return attributes;
}

[[nodiscard]] bool is_hidden_ntfs_name(const std::wstring& name) noexcept {
    return !name.empty() && name.front() == L'$';
}

[[nodiscard]] std::wstring make_disk_id(const std::uint32_t disk_number) {
    return std::wstring(kDiskPrefix) + std::to_wstring(disk_number);
}

[[nodiscard]] std::wstring make_volume_id(const std::uint32_t volume_index) {
    return std::wstring(kVolumePrefix) + std::to_wstring(volume_index);
}

[[nodiscard]] std::wstring make_ntfs_id(const std::uint32_t volume_index,
                                        const ntfs_api::NtfsFileReference& reference) {
    return std::wstring(kNtfsPrefix) + std::to_wstring(volume_index) + L":" +
           std::to_wstring(reference.record_number) + L":" +
           std::to_wstring(reference.sequence_number);
}

[[nodiscard]] std::wstring make_file_set_id(const std::uint64_t entry_id) {
    return std::wstring(kFileSetPrefix) + std::to_wstring(entry_id);
}

[[nodiscard]] bool is_root_folder(const std::wstring& folder_id) noexcept {
    return folder_id.empty();
}

[[nodiscard]] bool is_disk_id(const std::wstring& item_id) noexcept {
    return item_id.rfind(kDiskPrefix, 0) == 0;
}

[[nodiscard]] bool is_volume_id(const std::wstring& item_id) noexcept {
    return item_id.rfind(kVolumePrefix, 0) == 0;
}

[[nodiscard]] bool is_ntfs_id(const std::wstring& item_id) noexcept {
    return item_id.rfind(kNtfsPrefix, 0) == 0;
}

[[nodiscard]] bool is_file_set_id(const std::wstring& item_id) noexcept {
    return item_id.rfind(kFileSetPrefix, 0) == 0;
}

[[nodiscard]] std::uint32_t extract_disk_number(const std::wstring& item_id) {
    if (!is_disk_id(item_id)) {
        return 0;
    }
    return static_cast<std::uint32_t>(
        std::wcstoul(item_id.c_str() + wcslen(kDiskPrefix), nullptr, 10));
}

[[nodiscard]] std::uint32_t extract_volume_index(const std::wstring& item_id) {
    if (is_volume_id(item_id)) {
        return static_cast<std::uint32_t>(
            std::wcstoul(item_id.c_str() + wcslen(kVolumePrefix), nullptr, 10));
    }
    if (!is_ntfs_id(item_id)) {
        return 0;
    }
    const auto body = item_id.substr(wcslen(kNtfsPrefix));
    return static_cast<std::uint32_t>(std::wcstoul(body.c_str(), nullptr, 10));
}

[[nodiscard]] bool parse_ntfs_id(const std::wstring& item_id, std::uint32_t& volume_index,
                                 ntfs_api::NtfsFileReference& reference) {
    if (!is_ntfs_id(item_id)) {
        return false;
    }
    unsigned long long volume = 0;
    unsigned long long record = 0;
    unsigned long sequence = 0;
    if (swscanf_s(item_id.c_str(), L"ntfs:%llu:%llu:%lu", &volume, &record, &sequence) != 3) {
        return false;
    }
    volume_index = static_cast<std::uint32_t>(volume);
    reference.record_number = static_cast<std::uint64_t>(record);
    reference.sequence_number = static_cast<std::uint16_t>(sequence);
    return true;
}

[[nodiscard]] std::uint64_t extract_file_set_entry_id(const std::wstring& item_id) {
    if (!is_file_set_id(item_id)) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        std::wcstoull(item_id.c_str() + wcslen(kFileSetPrefix), nullptr, 10));
}

// Prefer "Label (G:)" — never append filesystem type (NTFS/ReFS/…).
[[nodiscard]] std::wstring extract_drive_letter(const std::wstring& mount_point) {
    if (mount_point.size() >= 2 && mount_point[1] == L':') {
        const wchar_t letter = mount_point[0];
        if ((letter >= L'A' && letter <= L'Z') || (letter >= L'a' && letter <= L'z')) {
            std::wstring drive(1, static_cast<wchar_t>(std::towupper(letter)));
            drive += L':';
            return drive;
        }
    }
    return {};
}

[[nodiscard]] std::wstring build_volume_display_name(const format::Volume& volume) {
    std::wstring drive;
    for (const auto& mount : volume.mount_points) {
        drive = extract_drive_letter(utf8_to_wide(mount));
        if (!drive.empty()) {
            break;
        }
    }

    std::wstring label = utf8_to_wide(volume.label);
    // Trim trailing spaces often present in volume labels.
    while (!label.empty() && (label.back() == L' ' || label.back() == L'\t')) {
        label.pop_back();
    }

    if (!label.empty() && !drive.empty()) {
        return label + L" (" + drive + L")";
    }
    if (!label.empty()) {
        return label;
    }
    if (!drive.empty()) {
        return drive;
    }
    if (!volume.mount_points.empty()) {
        auto mount = utf8_to_wide(volume.mount_points.front());
        while (!mount.empty() && (mount.back() == L'\\' || mount.back() == L'/')) {
            mount.pop_back();
        }
        if (!mount.empty()) {
            return mount;
        }
    }
    return format_shell_string_u32(IDS_VOLUME_FORMAT, volume.volume_index);
}

[[nodiscard]] std::wstring sanitize_file_name(std::wstring name) {
    for (auto& ch : name) {
        if (ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'/' || ch == L'\\' ||
            ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    if (name.empty()) {
        return L"item";
    }
    return name;
}

/// Direct open (materialize + ShellExecute) cap. Larger files must be copied out first.
/// Stricter than product S09 (16 GiB) which applies to future cache/copy quotas.
constexpr std::uint64_t kMaximumDirectOpenBytes = 1ULL * 1024ULL * 1024ULL * 1024ULL;

[[nodiscard]] bool exceeds_direct_open_limit(const std::uint64_t logical_size) noexcept {
    return logical_size > kMaximumDirectOpenBytes;
}

[[nodiscard]] std::wstring build_temp_path(const std::wstring& root_path,
                                           const std::wstring& item_id,
                                           const std::wstring& file_name) {
    wchar_t temp_dir[MAX_PATH]{};
    if (::GetTempPathW(static_cast<DWORD>(_countof(temp_dir)), temp_dir) == 0) {
        wcscpy_s(temp_dir, L"C:\\Windows\\Temp\\");
    }
    const std::size_t hash = std::hash<std::wstring>{}(root_path + L"|" + item_id);
    fs::path path(temp_dir);
    path /= L"AegraExplorer";
    path /= std::to_wstring(hash);
    path /= sanitize_file_name(file_name);
    return path.wstring();
}

[[nodiscard]] HRESULT hresult_from_error(const base::Error& error) {
    switch (error.code) {
    case base::ErrorCode::kNotFound:
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    case base::ErrorCode::kUnauthorized:
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    case base::ErrorCode::kCancelled:
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    case base::ErrorCode::kInvalidArgument:
        return E_INVALIDARG;
    case base::ErrorCode::kIoFailure:
        return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
    default:
        return E_FAIL;
    }
}

[[nodiscard]] ArchiveShellItem make_error_item(const std::wstring& message) {
    ArchiveShellItem item;
    item.itemId = kErrorItemId;
    item.displayName = message.empty() ? load_shell_string(IDS_ERR_UNABLE_OPEN) : message;
    item.attributes = file_attributes_shell();
    item.fileAttributes = FILE_ATTRIBUTE_NORMAL;
    item.isFolder = false;
    return item;
}

[[nodiscard]] ArchiveShellItem make_folder_item(std::wstring item_id, std::wstring display_name) {
    ArchiveShellItem item;
    item.itemId = std::move(item_id);
    item.displayName = std::move(display_name);
    item.attributes = folder_attributes();
    item.fileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    item.isFolder = true;
    return item;
}

[[nodiscard]] ArchiveShellItem make_ntfs_item(const std::uint32_t volume_index,
                                              const ntfs_api::NtfsEntry& entry) {
    ArchiveShellItem item;
    item.itemId = make_ntfs_id(volume_index, entry.reference);
    item.displayName = u16_to_wide(entry.name);
    item.size = entry.logical_size;
    item.modifiedTime = entry.modification_time;
    item.attributes = ntfs_shell_attributes(entry);
    if (is_hidden_ntfs_name(item.displayName)) {
        item.attributes |= SFGAO_HIDDEN;
    }
    item.fileAttributes =
        entry.file_attributes == 0
            ? (entry.is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL)
            : entry.file_attributes;
    if (entry.is_directory) {
        item.fileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    item.isFolder = entry.is_directory;
    return item;
}

class UniqueWin32Handle final {
  public:
    UniqueWin32Handle() noexcept = default;
    explicit UniqueWin32Handle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueWin32Handle() { reset(); }
    UniqueWin32Handle(const UniqueWin32Handle&) = delete;
    UniqueWin32Handle& operator=(const UniqueWin32Handle&) = delete;
    UniqueWin32Handle(UniqueWin32Handle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    UniqueWin32Handle& operator=(UniqueWin32Handle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    void reset() noexcept {
        if (valid()) {
            ::CloseHandle(handle_);
        }
        handle_ = INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] HRESULT write_all(HANDLE handle, const std::byte* data, const std::size_t size) {
    std::size_t written_total = 0;
    while (written_total < size) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(size - written_total, static_cast<std::size_t>(1U << 20)));
        DWORD written = 0;
        if (!::WriteFile(handle, data + written_total, chunk, &written, nullptr)) {
            return HRESULT_FROM_WIN32(::GetLastError());
        }
        if (written == 0) {
            return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
        }
        written_total += written;
    }
    return S_OK;
}

} // namespace

// Opaque volume browse state (random reader + NTFS parser). Kept out of the header.
class VolumeBrowseContext final {
  public:
    std::unique_ptr<archive_api::PersonalArchiveVolumeRandomReader> random_reader;
    std::unique_ptr<ntfs_api::NtfsVolumeReader> ntfs;
};

class VolumeSetContext final {
  public:
    std::unique_ptr<archive_api::PersonalArchiveReader> single;
    std::unique_ptr<archive_api::PersonalArchiveChainReader> chain;

    [[nodiscard]] ports::IRecoveryPointReader* reader() const noexcept {
        if (chain != nullptr) {
            return chain.get();
        }
        return single.get();
    }

    [[nodiscard]] const format::Manifest* manifest() const noexcept {
        if (chain != nullptr) {
            return &chain->manifest();
        }
        if (single != nullptr) {
            return &single->manifest();
        }
        return nullptr;
    }
};

class FileSetContext final {
  public:
    std::unique_ptr<archive_api::PersonalFileArchiveReader> single;
    std::unique_ptr<archive_api::PersonalFileArchiveChainReader> chain;

    [[nodiscard]] ports::IFileRecoveryPointReader* reader() const noexcept {
        if (chain != nullptr) {
            return chain.get();
        }
        return single.get();
    }
};
[[nodiscard]] bool is_zero_uuid(const std::array<std::byte, 16>& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](const std::byte item) { return item == std::byte{0}; });
}

[[nodiscard]] std::wstring archive_leaf_name(const std::wstring& root_path) {
    const fs::path path(root_path);
    auto name = path.filename().wstring();
    return name.empty() ? root_path : name;
}

/// Auth failures that should run the Shell password dialog. Only ErrorCode — never English text.
[[nodiscard]] bool is_password_required_error(const base::Error& error) noexcept {
    return error.code == base::ErrorCode::kUnauthorized;
}

/// @p supplied_password_empty distinguishes "needs password" vs "wrong password" without parsing
/// adapter message text (both use kUnauthorized).
[[nodiscard]] std::wstring message_for_open_error(const base::Error& error,
                                                  const bool supplied_password_empty) {
    if (is_password_required_error(error)) {
        return load_shell_string(supplied_password_empty ? IDS_ERR_ARCHIVE_ENCRYPTED
                                                         : IDS_ERR_INVALID_PASSWORD);
    }
    if (error.code == base::ErrorCode::kCancelled) {
        return load_shell_string(IDS_ERR_PASSWORD_CANCELLED);
    }
    // Stable product code from chain resolver (prefix form: "shell.parent_missing:...").
    if (error.code == base::ErrorCode::kNotFound &&
        error.message.rfind("shell.parent_missing", 0) == 0) {
        return load_shell_string(IDS_ERR_PARENT_MISSING);
    }
    if (error.code == base::ErrorCode::kUnsupportedVersion ||
        error.message.rfind("shell.unsupported_content_kind", 0) == 0) {
        return load_shell_string(IDS_ERR_UNSUPPORTED_VERSION);
    }
    return load_shell_string(IDS_ERR_UNABLE_OPEN);
}

[[nodiscard]] archive_api::ArchiveOpenRequest make_open_request(const fs::path& source,
                                                                const std::string_view password) {
    archive_api::ArchiveOpenRequest request;
    request.source = source;
    request.password = password;
    return request;
}

[[nodiscard]] archive_api::ArchiveChainOpenRequest
make_chain_request(const std::vector<fs::path>& layers, const std::string_view password) {
    archive_api::ArchiveChainOpenRequest request;
    request.layers.reserve(layers.size());
    for (const auto& layer : layers) {
        request.layers.push_back(make_open_request(layer, password));
    }
    return request;
}

ArchiveShellModel& ArchiveShellModel::Instance() {
    static ArchiveShellModel model;
    return model;
}
ArchiveShellModel::~ArchiveShellModel() = default;

ArchiveShellModel::ArchiveSession&
ArchiveShellModel::GetSessionLocked(const std::wstring& rootPath) {
    const std::wstring key = normalize_session_key(rootPath);
    auto it = sessions_.find(key);
    if (it != sessions_.end()) {
        shell_debug_log(L"GetSession hit key=[" + key + L"] raw=[" + rootPath + L"] opened=" +
                        (it->second->opened ? L"1" : L"0") + L" sessions=" +
                        std::to_wstring(sessions_.size()));
        return *it->second;
    }
    auto session = std::make_unique<ArchiveSession>();
    // Keep the first-seen path for I/O/display; map key is normalized so path variants share
    // one session (and one credential), matching backup same-root password reuse.
    session->root_path = trim_trailing_slash(rootPath);
    auto [inserted, _] = sessions_.emplace(key, std::move(session));
    shell_debug_log(L"GetSession NEW key=[" + key + L"] raw=[" + rootPath + L"] sessions=" +
                    std::to_wstring(sessions_.size()));
    return *inserted->second;
}

bool ArchiveShellModel::DecryptSessionCredential(ArchiveSession& session,
                                                 SecurePassword& plaintext) {
    if (session.credential.empty()) {
        return false;
    }
    if (session.credential.unprotect(plaintext)) {
        return true;
    }
    session.credential.clear();
    shell_debug_log(L"DPAPI credential decrypt failed path=[" + session.root_path + L"]");
    return false;
}

void ArchiveShellModel::ProtectSessionCredential(ArchiveSession& session,
                                                 const std::string_view plaintext) {
    if (plaintext.empty()) {
        session.credential.clear();
        return;
    }
    if (!session.credential.protect(plaintext)) {
        shell_debug_log(L"DPAPI credential protect failed path=[" + session.root_path + L"]");
    }
}

HRESULT ArchiveShellModel::EnsureSessionOpened(ArchiveSession& session) {
    if (session.opened) {
        return S_OK;
    }
    if (session.open_failed) {
        shell_debug_log(L"EnsureSession sticky fail hr=0x" +
                        std::to_wstring(static_cast<unsigned long>(session.open_hr)) + L" path=[" +
                        session.root_path + L"]");
        return session.open_hr;
    }
    if (session.opening) {
        shell_debug_log(L"EnsureSession BUSY (nested) path=[" + session.root_path + L"]");
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    if (session.root_path.empty()) {
        session.error = load_shell_string(IDS_ERR_PATH_EMPTY);
        session.open_failed = true;
        session.open_hr = E_INVALIDARG;
        return session.open_hr;
    }
    session.opening = true;
    struct OpeningGuard final {
        bool& flag;
        ~OpeningGuard() { flag = false; }
    } opening_guard{session.opening};
    SecurePassword password;
    if (DecryptSessionCredential(session, password)) {
        shell_debug_log(L"decrypted DPAPI session credential path=[" + session.root_path + L"]");
    } else {
        shell_debug_log(L"seed password empty path=[" + session.root_path + L"]");
    }
    const std::wstring root_path = session.root_path;
    shell_debug_log(L"EnsureSession open begin path=[" + root_path + L"] has_seed=" +
                    (password.empty() ? L"0" : L"1"));
    const HRESULT open_hr = OpenSessionWithCredential(session, password.view());
    if (session.opened) {
        ProtectSessionCredential(session, password.view());
        password.clear();
        return S_OK;
    }
    if (session.open_failed || open_hr != HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
        session.credential.clear();
        password.clear();
        return session.open_failed ? session.open_hr : open_hr;
    }

    session.credential.clear();
    const PasswordValidator validator = [this, &session](const std::string_view candidate) {
        return OpenSessionWithCredential(session, candidate);
    };
    const auto dialog = prompt_archive_password(archive_leaf_name(root_path), password, validator);
    if (dialog == PasswordDialogResult::kOk && session.opened) {
        ProtectSessionCredential(session, password.view());
        password.clear();
        return S_OK;
    }
    password.clear();
    session.credential.clear();
    if (dialog == PasswordDialogResult::kFailed) {
        return session.open_failed ? session.open_hr : E_FAIL;
    }

    // AIVImage returns a null ShellView on IDCANCEL. Keep cancellation non-sticky so a later,
    // explicit open can prompt again, while this navigation attempt simply remains in place.
    session.error.clear();
    session.opened = false;
    session.open_failed = false;
    session.open_hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    shell_debug_log(L"EnsureSession CANCEL path=[" + root_path + L"]");
    return session.open_hr;
}

HRESULT ArchiveShellModel::PrepareFolderView(const std::wstring& rootPath) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ArchiveSession& session = GetSessionLocked(rootPath);
    return EnsureSessionOpened(session);
}

namespace {

struct OpenReaderOutcome final {
    enum class Kind : std::uint8_t {
        kUnauthorized = 1,
        kStickyFailure = 2,
        kVolumeSet = 3,
        kFileSet = 4,
    };

    Kind kind{Kind::kStickyFailure};
    HRESULT hr{E_FAIL};
    std::wstring error;
    std::unique_ptr<VolumeSetContext> volume_set;
    std::unique_ptr<FileSetContext> file_set;
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::string, std::wstring, std::wstring>>
        volumes; // disk, index, fs, display, root_id
};

[[nodiscard]] OpenReaderOutcome unauthorized_outcome(const base::Error& error,
                                                     const bool supplied_password_empty) {
    OpenReaderOutcome outcome;
    outcome.kind = OpenReaderOutcome::Kind::kUnauthorized;
    // Always ACCESS_DENIED so EnsureSessionOpened can run the password dialog.
    outcome.hr = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    outcome.error = message_for_open_error(error, supplied_password_empty);
    return outcome;
}

[[nodiscard]] OpenReaderOutcome sticky_outcome(const base::Error& error,
                                               const bool supplied_password_empty = false) {
    OpenReaderOutcome outcome;
    outcome.kind = OpenReaderOutcome::Kind::kStickyFailure;
    outcome.hr = hresult_from_error(error);
    outcome.error = message_for_open_error(error, supplied_password_empty);
    return outcome;
}

[[nodiscard]] OpenReaderOutcome
build_volume_set(const fs::path& tip_path, const std::string_view password,
                 std::unique_ptr<archive_api::PersonalArchiveReader> tip) {
    const bool password_empty = password.empty();
    const auto tip_file_uuid = tip->identity().file_uuid;
    const bool incremental = !is_zero_uuid(tip->identity().parent_uuid);
    auto volume_set = std::make_unique<VolumeSetContext>();
    if (!incremental) {
        volume_set->single = std::move(tip);
    } else {
        tip.reset();
        auto chain_paths = resolve_managed_archive_chain(tip_path, tip_file_uuid);
        if (!chain_paths) {
            return sticky_outcome(chain_paths.error(), password_empty);
        }
        auto chain = archive_api::PersonalArchiveChainReader::open(
            make_chain_request(chain_paths.value().layer_paths, password));
        if (!chain) {
            return is_password_required_error(chain.error())
                       ? unauthorized_outcome(chain.error(), password_empty)
                       : sticky_outcome(chain.error(), password_empty);
        }
        volume_set->chain = std::move(chain).value();
    }
    const format::Manifest* manifest = volume_set->manifest();
    if (manifest == nullptr) {
        OpenReaderOutcome outcome;
        outcome.kind = OpenReaderOutcome::Kind::kStickyFailure;
        outcome.hr = E_FAIL;
        outcome.error = load_shell_string(IDS_ERR_UNABLE_OPEN);
        return outcome;
    }
    OpenReaderOutcome outcome;
    outcome.kind = OpenReaderOutcome::Kind::kVolumeSet;
    outcome.hr = S_OK;
    for (const auto& volume : manifest->volumes) {
        outcome.volumes.emplace_back(
            volume.extents.empty() ? 0U : volume.extents.front().disk_number, volume.volume_index,
            volume.filesystem, build_volume_display_name(volume),
            make_volume_id(volume.volume_index));
    }
    if (outcome.volumes.empty()) {
        outcome.error = load_shell_string(IDS_ERR_NO_VOLUMES);
    }
    outcome.volume_set = std::move(volume_set);
    return outcome;
}

[[nodiscard]] OpenReaderOutcome
build_file_set(const fs::path& tip_path, const std::string_view password,
               std::unique_ptr<archive_api::PersonalFileArchiveReader> tip) {
    const bool password_empty = password.empty();
    const auto tip_file_uuid = tip->identity().file_uuid;
    const bool incremental = !is_zero_uuid(tip->identity().parent_uuid);
    auto file_set = std::make_unique<FileSetContext>();
    if (!incremental) {
        file_set->single = std::move(tip);
    } else {
        tip.reset();
        auto chain_paths = resolve_managed_archive_chain(tip_path, tip_file_uuid);
        if (!chain_paths) {
            return sticky_outcome(chain_paths.error(), password_empty);
        }
        auto chain = archive_api::PersonalFileArchiveChainReader::open(
            make_chain_request(chain_paths.value().layer_paths, password));
        if (!chain) {
            return is_password_required_error(chain.error())
                       ? unauthorized_outcome(chain.error(), password_empty)
                       : sticky_outcome(chain.error(), password_empty);
        }
        file_set->chain = std::move(chain).value();
    }
    OpenReaderOutcome outcome;
    outcome.kind = OpenReaderOutcome::Kind::kFileSet;
    outcome.hr = S_OK;
    outcome.file_set = std::move(file_set);
    return outcome;
}

[[nodiscard]] OpenReaderOutcome open_archive_readers(const fs::path& tip_path,
                                                     const std::string_view password) {
    const bool password_empty = password.empty();
    const auto request = make_open_request(tip_path, password);

    // Authenticate Header and Manifest before dispatch. Open exactly one matching reader and
    // never fall back to another content kind on failure (ADR-0023).
    auto metadata = archive_api::authenticate_archive_metadata(request);
    if (!metadata) {
        return is_password_required_error(metadata.error())
                   ? unauthorized_outcome(metadata.error(), password_empty)
                   : sticky_outcome(metadata.error(), password_empty);
    }

    if (metadata.value().content_kind == format::personal_archive::kContentKindVolumeSet) {
        auto volume_open = archive_api::PersonalArchiveReader::open(request);
        if (!volume_open) {
            return is_password_required_error(volume_open.error())
                       ? unauthorized_outcome(volume_open.error(), password_empty)
                       : sticky_outcome(volume_open.error(), password_empty);
        }
        if (volume_open.value()->manifest().content_kind != format::kManifestContentKindVolumeSet) {
            return sticky_outcome(
                base::Error{base::ErrorCode::kCorruptData, "shell.archive_corrupt"},
                password_empty);
        }
        return build_volume_set(tip_path, password, std::move(volume_open).value());
    }

    if (metadata.value().content_kind == format::personal_archive::kContentKindFileSet) {
        auto file_open = archive_api::PersonalFileArchiveReader::open(request);
        if (!file_open) {
            return is_password_required_error(file_open.error())
                       ? unauthorized_outcome(file_open.error(), password_empty)
                       : sticky_outcome(file_open.error(), password_empty);
        }
        if (file_open.value()->manifest().content_kind != format::kManifestContentKindFileSet) {
            return sticky_outcome(
                base::Error{base::ErrorCode::kCorruptData, "shell.archive_corrupt"},
                password_empty);
        }
        return build_file_set(tip_path, password, std::move(file_open).value());
    }

    return sticky_outcome(
        base::Error{base::ErrorCode::kUnsupportedVersion, "shell.unsupported_content_kind"},
        password_empty);
}

} // namespace

HRESULT ArchiveShellModel::OpenSessionWithCredential(ArchiveSession& session,
                                                     const std::string_view password) {
    // Caller holds mutex_. Password material is only the stack SecurePassword view.
    session.error.clear();
    session.volumes.clear();
    session.volume_set.reset();
    session.file_set.reset();
    session.content_kind = ContentKind::kUnknown;
    session.opened = false;

    auto outcome = open_archive_readers(fs::path(session.root_path), password);
    session.error = std::move(outcome.error);
    if (outcome.kind == OpenReaderOutcome::Kind::kUnauthorized) {
        return outcome.hr;
    }
    if (outcome.kind == OpenReaderOutcome::Kind::kStickyFailure) {
        session.open_failed = true;
        session.open_hr = outcome.hr;
        return session.open_hr;
    }
    if (outcome.kind == OpenReaderOutcome::Kind::kVolumeSet) {
        for (auto& entry : outcome.volumes) {
            VolumeState state;
            state.disk_number = std::get<0>(entry);
            state.volume_index = std::get<1>(entry);
            state.filesystem_utf8 = std::move(std::get<2>(entry));
            state.display_name = std::move(std::get<3>(entry));
            state.root_item_id = std::move(std::get<4>(entry));
            session.volumes.push_back(std::move(state));
        }
        session.volume_set = std::move(outcome.volume_set);
        session.content_kind = ContentKind::kVolumeSet;
        session.opened = true;
        return S_OK;
    }
    session.file_set = std::move(outcome.file_set);
    session.content_kind = ContentKind::kFileSet;
    session.opened = true;
    return S_OK;
}

HRESULT ArchiveShellModel::EnsureVolumeBrowseLocked(ArchiveSession& session, VolumeState& volume) {
    if (volume.browse_attempted) {
        return volume.browse_result;
    }
    volume.browse_attempted = true;
    if (session.volume_set == nullptr || session.volume_set->reader() == nullptr ||
        session.volume_set->manifest() == nullptr) {
        volume.browse_result = E_UNEXPECTED;
        return volume.browse_result;
    }
    if (!is_ntfs_filesystem(volume.filesystem_utf8)) {
        volume.browse_result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return volume.browse_result;
    }

    auto random = archive_api::PersonalArchiveVolumeRandomReader::open(
        *session.volume_set->reader(), *session.volume_set->manifest(), volume.volume_index);
    if (!random) {
        volume.browse_result = hresult_from_error(random.error());
        return volume.browse_result;
    }
    auto browse = std::make_unique<VolumeBrowseContext>();
    browse->random_reader = std::move(random).value();
    auto ntfs = ntfs_api::NtfsVolumeReader::open(*browse->random_reader, base::CancellationToken{});
    if (!ntfs) {
        volume.browse_result = hresult_from_error(ntfs.error());
        return volume.browse_result;
    }
    browse->ntfs = std::move(ntfs).value();
    volume.browse = std::move(browse);
    volume.browse_result = S_OK;
    return S_OK;
}

HRESULT ArchiveShellModel::EnumRootDisks(const ArchiveSession& session,
                                         std::vector<ArchiveShellItem>& items) {
    std::vector<std::uint32_t> disks;
    for (const auto& volume : session.volumes) {
        if (std::find(disks.begin(), disks.end(), volume.disk_number) == disks.end()) {
            disks.push_back(volume.disk_number);
        }
    }
    std::sort(disks.begin(), disks.end());
    items.reserve(disks.size());
    for (const auto disk : disks) {
        items.push_back(
            make_folder_item(make_disk_id(disk), format_shell_string_u32(IDS_DISK_FORMAT, disk)));
    }
    if (items.empty() && !session.error.empty()) {
        items.push_back(make_error_item(session.error));
    }
    return S_OK;
}

HRESULT ArchiveShellModel::EnumDiskVolumes(const ArchiveSession& session,
                                           const std::uint32_t disk_number,
                                           std::vector<ArchiveShellItem>& items) {
    for (const auto& volume : session.volumes) {
        if (volume.disk_number == disk_number) {
            items.push_back(make_folder_item(volume.root_item_id, volume.display_name));
        }
    }
    return S_OK;
}

HRESULT ArchiveShellModel::EnumNtfsChildren(ArchiveSession& session, const std::wstring& folderId,
                                            std::vector<ArchiveShellItem>& items) {
    std::uint32_t volume_index = 0;
    ntfs_api::NtfsFileReference directory{};
    if (is_volume_id(folderId)) {
        volume_index = extract_volume_index(folderId);
        directory.record_number = ntfs_api::kNtfsRootFileReference;
        directory.sequence_number = 0; // open describe will resolve sequence if needed
    } else if (!parse_ntfs_id(folderId, volume_index, directory)) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    auto volume_it =
        std::find_if(session.volumes.begin(), session.volumes.end(),
                     [&](const VolumeState& v) { return v.volume_index == volume_index; });
    if (volume_it == session.volumes.end()) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    const HRESULT browse_hr = EnsureVolumeBrowseLocked(session, *volume_it);
    if (FAILED(browse_hr)) {
        if (is_volume_id(folderId)) {
            items.push_back(make_error_item(is_ntfs_filesystem(volume_it->filesystem_utf8)
                                                ? load_shell_string(IDS_ERR_NTFS_PARSE)
                                                : load_shell_string(IDS_ERR_NTFS_ONLY)));
            return S_OK;
        }
        return browse_hr;
    }

    auto& ntfs = *volume_it->browse->ntfs;
    // Volume root: list MFT #5. Directory items already carry sequence in the id.
    if (is_volume_id(folderId)) {
        directory.record_number = ntfs_api::kNtfsRootFileReference;
        auto root_entry =
            ntfs.describe_entry(ntfs_api::NtfsFileReference{ntfs_api::kNtfsRootFileReference, 0},
                                base::CancellationToken{});
        if (root_entry) {
            directory = root_entry.value().reference;
        } else {
            // Fall back to record 5 with sequence 0; list_directory may still succeed.
            directory = {ntfs_api::kNtfsRootFileReference, 1};
        }
    }

    std::optional<std::string> continuation;
    do {
        auto page = ntfs.list_directory(directory, ntfs_api::kMaximumDirectoryPage, continuation,
                                        base::CancellationToken{});
        if (!page) {
            return hresult_from_error(page.error());
        }
        for (const auto& entry : page.value().items) {
            if (entry.name == u"." || entry.name == u"..") {
                continue;
            }
            items.push_back(make_ntfs_item(volume_index, entry));
        }
        continuation = page.value().continuation_token;
    } while (continuation.has_value());
    return S_OK;
}

HRESULT ArchiveShellModel::EnumFileSetChildren(ArchiveSession& session,
                                               const std::wstring& folderId,
                                               std::vector<ArchiveShellItem>& items) {
    if (session.file_set == nullptr || session.file_set->reader() == nullptr) {
        return E_UNEXPECTED;
    }
    std::uint64_t parent_id = 0;
    if (!folderId.empty()) {
        if (!is_file_set_id(folderId)) {
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
        parent_id = extract_file_set_entry_id(folderId);
    }

    auto& reader = *session.file_set->reader();
    std::optional<std::string> continuation;
    do {
        auto page = reader.list_children(parent_id, 256, continuation, base::CancellationToken{});
        if (!page) {
            return hresult_from_error(page.error());
        }
        for (const auto& summary : page.value().items) {
            const auto entry_id = static_cast<std::uint64_t>(std::stoull(summary.entry_id));
            auto described = reader.describe_entry(entry_id, base::CancellationToken{});
            if (!described) {
                continue;
            }
            const auto& entry = described.value();
            ArchiveShellItem item;
            item.itemId = make_file_set_id(entry.entry_id);
            item.displayName = encoded_name_to_wide(entry.name);
            item.size = entry.logical_size;
            item.modifiedTime = entry.write_time;
            const bool is_dir = entry.kind == contracts::FileEntryKind::kDirectory;
            item.isFolder = is_dir;
            item.attributes = is_dir ? folder_attributes() : file_attributes_shell();
            item.fileAttributes = is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            items.push_back(std::move(item));
        }
        continuation = page.value().continuation_token;
    } while (continuation.has_value());
    return S_OK;
}

HRESULT ArchiveShellModel::EnumChildren(const std::wstring& rootPath, const std::wstring& folderId,
                                        std::vector<ArchiveShellItem>& items) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    items.clear();
    shell_debug_log(L"EnumChildren folder=[" + folderId + L"] raw=[" + rootPath + L"]");

    ArchiveSession& session = GetSessionLocked(rootPath);
    const HRESULT open_hr = EnsureSessionOpened(session);
    if (FAILED(open_hr)) {
        // Nested open while password UI is up — leave the view empty; outer call finishes.
        if (open_hr == HRESULT_FROM_WIN32(ERROR_BUSY)) {
            return S_OK;
        }
        return open_hr;
    }

    if (session.content_kind == ContentKind::kFileSet) {
        if (is_root_folder(folderId) || is_file_set_id(folderId)) {
            return EnumFileSetChildren(session, folderId, items);
        }
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    if (is_root_folder(folderId)) {
        return EnumRootDisks(session, items);
    }
    if (is_disk_id(folderId)) {
        return EnumDiskVolumes(session, extract_disk_number(folderId), items);
    }
    if (is_volume_id(folderId) || is_ntfs_id(folderId)) {
        return EnumNtfsChildren(session, folderId, items);
    }
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

HRESULT ArchiveShellModel::GetVolumeSetItemInfo(ArchiveSession& session, const std::wstring& itemId,
                                                ArchiveShellItem& item) {
    if (is_disk_id(itemId)) {
        const auto disk = extract_disk_number(itemId);
        const auto it = std::find_if(session.volumes.begin(), session.volumes.end(),
                                     [&](const VolumeState& v) { return v.disk_number == disk; });
        if (it == session.volumes.end()) {
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
        item = make_folder_item(make_disk_id(disk), format_shell_string_u32(IDS_DISK_FORMAT, disk));
        return S_OK;
    }
    if (is_volume_id(itemId)) {
        const auto volume_index = extract_volume_index(itemId);
        const auto it =
            std::find_if(session.volumes.begin(), session.volumes.end(),
                         [&](const VolumeState& v) { return v.volume_index == volume_index; });
        if (it == session.volumes.end()) {
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
        item = make_folder_item(it->root_item_id, it->display_name);
        return S_OK;
    }
    if (!is_ntfs_id(itemId)) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    std::uint32_t volume_index = 0;
    ntfs_api::NtfsFileReference reference{};
    if (!parse_ntfs_id(itemId, volume_index, reference)) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    auto volume_it =
        std::find_if(session.volumes.begin(), session.volumes.end(),
                     [&](const VolumeState& v) { return v.volume_index == volume_index; });
    if (volume_it == session.volumes.end()) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    const HRESULT browse_hr = EnsureVolumeBrowseLocked(session, *volume_it);
    if (FAILED(browse_hr)) {
        return browse_hr;
    }
    auto described = volume_it->browse->ntfs->describe_entry(reference, base::CancellationToken{});
    if (!described) {
        return hresult_from_error(described.error());
    }
    item = make_ntfs_item(volume_index, described.value());
    return S_OK;
}

HRESULT ArchiveShellModel::GetFileSetItemInfo(ArchiveSession& session, const std::wstring& itemId,
                                              ArchiveShellItem& item) {
    if (!is_file_set_id(itemId) || session.file_set == nullptr ||
        session.file_set->reader() == nullptr) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    const auto entry_id = extract_file_set_entry_id(itemId);
    auto described =
        session.file_set->reader()->describe_entry(entry_id, base::CancellationToken{});
    if (!described) {
        return hresult_from_error(described.error());
    }
    const auto& entry = described.value();
    item.itemId = make_file_set_id(entry.entry_id);
    item.displayName = encoded_name_to_wide(entry.name);
    item.size = entry.logical_size;
    item.modifiedTime = entry.write_time;
    const bool is_dir = entry.kind == contracts::FileEntryKind::kDirectory;
    item.isFolder = is_dir;
    item.attributes = is_dir ? folder_attributes() : file_attributes_shell();
    item.fileAttributes = is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    return S_OK;
}

HRESULT ArchiveShellModel::GetItemInfo(const std::wstring& rootPath, const std::wstring& itemId,
                                       ArchiveShellItem& item) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    item = ArchiveShellItem{};
    ArchiveSession& session = GetSessionLocked(rootPath);
    const HRESULT open_hr = EnsureSessionOpened(session);
    if (FAILED(open_hr)) {
        return open_hr;
    }
    if (itemId == kErrorItemId) {
        item = make_error_item(session.error);
        return S_OK;
    }
    if (session.content_kind == ContentKind::kFileSet) {
        return GetFileSetItemInfo(session, itemId, item);
    }
    return GetVolumeSetItemInfo(session, itemId, item);
}

HRESULT ArchiveShellModel::GetInfoTip(const std::wstring& rootPath, const std::wstring& itemId,
                                      std::wstring& text) {
    ArchiveShellItem item;
    const HRESULT hr = GetItemInfo(rootPath, itemId, item);
    if (FAILED(hr)) {
        return hr;
    }
    std::wstringstream stream;
    stream << (item.isFolder ? load_shell_string(IDS_TIP_ARCHIVE_FOLDER)
                             : load_shell_string(IDS_TIP_ARCHIVE_FILE))
           << L": " << item.displayName;
    if (!item.isFolder) {
        stream << L"\n" << format_shell_string(IDS_TIP_SIZE, std::to_wstring(item.size));
    }
    stream << L"\n" << format_shell_string(IDS_TIP_SOURCE, rootPath);
    text = stream.str();
    return S_OK;
}

HRESULT ArchiveShellModel::OpenNtfsFile(ArchiveSession& session, HWND hwnd,
                                        const std::wstring& itemId) {
    std::uint32_t volume_index = 0;
    ntfs_api::NtfsFileReference reference{};
    if (!parse_ntfs_id(itemId, volume_index, reference)) {
        return E_INVALIDARG;
    }
    auto volume_it =
        std::find_if(session.volumes.begin(), session.volumes.end(),
                     [&](const VolumeState& v) { return v.volume_index == volume_index; });
    if (volume_it == session.volumes.end()) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    const HRESULT browse_hr = EnsureVolumeBrowseLocked(session, *volume_it);
    if (FAILED(browse_hr)) {
        return browse_hr;
    }
    auto& ntfs = *volume_it->browse->ntfs;
    auto described = ntfs.describe_entry(reference, base::CancellationToken{});
    if (!described) {
        return hresult_from_error(described.error());
    }
    if (described.value().is_directory) {
        return E_INVALIDARG;
    }

    const std::uint64_t total = described.value().logical_size;
    if (exceeds_direct_open_limit(total)) {
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }

    const std::wstring display = u16_to_wide(described.value().name);
    const std::wstring temp_path = build_temp_path(session.root_path, itemId, display);
    std::error_code ec;
    fs::create_directories(fs::path(temp_path).parent_path(), ec);
    if (ec) {
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }

    UniqueWin32Handle file(::CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return HRESULT_FROM_WIN32(::GetLastError());
    }

    std::uint64_t offset = 0;
    std::vector<std::byte> buffer(ntfs_api::kMaximumStreamReadBytes);
    while (offset < total) {
        const auto to_read = static_cast<std::size_t>(
            (std::min)(static_cast<std::uint64_t>(buffer.size()), total - offset));
        auto read = ntfs.read_file(reference, offset, std::span<std::byte>(buffer.data(), to_read),
                                   base::CancellationToken{});
        if (!read) {
            return hresult_from_error(read.error());
        }
        if (read.value() == 0) {
            break;
        }
        const HRESULT write_hr = write_all(file.get(), buffer.data(), read.value());
        if (FAILED(write_hr)) {
            return write_hr;
        }
        offset += read.value();
    }
    file.reset();

    const HINSTANCE result =
        ::ShellExecuteW(hwnd, L"open", temp_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        return HRESULT_FROM_WIN32(static_cast<DWORD>(reinterpret_cast<INT_PTR>(result)));
    }
    return S_OK;
}

HRESULT ArchiveShellModel::OpenFileSetFile(ArchiveSession& session, HWND hwnd,
                                           const std::wstring& itemId) {
    if (session.file_set == nullptr || session.file_set->reader() == nullptr) {
        return E_UNEXPECTED;
    }
    const auto entry_id = extract_file_set_entry_id(itemId);
    auto described =
        session.file_set->reader()->describe_entry(entry_id, base::CancellationToken{});
    if (!described) {
        return hresult_from_error(described.error());
    }
    if (described.value().kind == contracts::FileEntryKind::kDirectory) {
        return E_INVALIDARG;
    }
    if (described.value().streams.empty()) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    const auto& stream_desc = described.value().streams.front();
    if (exceeds_direct_open_limit(stream_desc.logical_size)) {
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }

    const std::wstring display = encoded_name_to_wide(described.value().name);
    const std::wstring temp_path = build_temp_path(session.root_path, itemId, display);
    std::error_code ec;
    fs::create_directories(fs::path(temp_path).parent_path(), ec);
    if (ec) {
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }

    UniqueWin32Handle file(::CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return HRESULT_FROM_WIN32(::GetLastError());
    }

    const std::uint64_t total = stream_desc.logical_size;
    std::uint64_t offset = 0;
    std::vector<std::byte> buffer(1U << 20);
    while (offset < total) {
        const auto to_read = static_cast<std::uint64_t>(
            (std::min)(static_cast<std::uint64_t>(buffer.size()), total - offset));
        ports::FileStreamReadRequest request;
        request.stream_index = stream_desc.stream_index;
        request.offset = offset;
        request.size = to_read;
        auto read = session.file_set->reader()->read_stream(
            request, std::span<std::byte>(buffer.data(), static_cast<std::size_t>(to_read)),
            base::CancellationToken{});
        if (!read) {
            return hresult_from_error(read.error());
        }
        if (read.value() == 0) {
            break;
        }
        const HRESULT write_hr = write_all(file.get(), buffer.data(), read.value());
        if (FAILED(write_hr)) {
            return write_hr;
        }
        offset += read.value();
    }
    file.reset();

    const HINSTANCE result =
        ::ShellExecuteW(hwnd, L"open", temp_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        return HRESULT_FROM_WIN32(static_cast<DWORD>(reinterpret_cast<INT_PTR>(result)));
    }
    return S_OK;
}

HRESULT ArchiveShellModel::OpenItem(const std::wstring& rootPath, HWND hwnd,
                                    const std::wstring& itemId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ArchiveSession& session = GetSessionLocked(rootPath);
    const HRESULT open_hr = EnsureSessionOpened(session);
    if (FAILED(open_hr)) {
        return open_hr;
    }
    if (session.content_kind == ContentKind::kFileSet) {
        if (!is_file_set_id(itemId)) {
            return E_INVALIDARG;
        }
        return OpenFileSetFile(session, hwnd, itemId);
    }
    if (!is_ntfs_id(itemId)) {
        return E_INVALIDARG;
    }
    return OpenNtfsFile(session, hwnd, itemId);
}

HRESULT ArchiveShellModel::ReadNtfsFileDataLocked(ArchiveSession& session,
                                                  const std::wstring& itemId,
                                                  const ULONGLONG offset, void* buffer,
                                                  const DWORD length, DWORD* bytesRead) {
    std::uint32_t volume_index = 0;
    ntfs_api::NtfsFileReference reference{};
    if (!parse_ntfs_id(itemId, volume_index, reference)) {
        return E_INVALIDARG;
    }
    auto volume_it =
        std::find_if(session.volumes.begin(), session.volumes.end(),
                     [&](const VolumeState& v) { return v.volume_index == volume_index; });
    if (volume_it == session.volumes.end()) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    const HRESULT browse_hr = EnsureVolumeBrowseLocked(session, *volume_it);
    if (FAILED(browse_hr)) {
        return browse_hr;
    }
    auto described = volume_it->browse->ntfs->describe_entry(reference, base::CancellationToken{});
    if (!described) {
        return hresult_from_error(described.error());
    }
    if (described.value().is_directory) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    if (offset >= described.value().logical_size || length == 0) {
        return S_OK;
    }
    const auto remaining = described.value().logical_size - offset;
    const auto total_wanted =
        static_cast<std::size_t>((std::min)(static_cast<std::uint64_t>(length), remaining));
    // NTFS adapter rejects reads larger than kMaximumStreamReadBytes — loop in chunks.
    auto* out = static_cast<std::byte*>(buffer);
    std::size_t total_read = 0;
    while (total_read < total_wanted) {
        const auto chunk = (std::min)(static_cast<std::size_t>(ntfs_api::kMaximumStreamReadBytes),
                                      total_wanted - total_read);
        auto read = volume_it->browse->ntfs->read_file(
            reference, offset + total_read, std::span<std::byte>(out + total_read, chunk),
            base::CancellationToken{});
        if (!read) {
            return hresult_from_error(read.error());
        }
        if (read.value() == 0) {
            break;
        }
        total_read += read.value();
        if (read.value() < chunk) {
            break;
        }
    }
    *bytesRead = static_cast<DWORD>(total_read);
    return S_OK;
}

HRESULT ArchiveShellModel::ReadFileSetDataLocked(ArchiveSession& session,
                                                 const std::wstring& itemId, const ULONGLONG offset,
                                                 void* buffer, const DWORD length,
                                                 DWORD* bytesRead) {
    if (session.file_set == nullptr || session.file_set->reader() == nullptr) {
        return E_UNEXPECTED;
    }
    const auto entry_id = extract_file_set_entry_id(itemId);
    auto described =
        session.file_set->reader()->describe_entry(entry_id, base::CancellationToken{});
    if (!described) {
        return hresult_from_error(described.error());
    }
    if (described.value().kind == contracts::FileEntryKind::kDirectory ||
        described.value().streams.empty()) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    const auto& stream_desc = described.value().streams.front();
    if (offset >= stream_desc.logical_size || length == 0) {
        return S_OK;
    }
    const auto remaining = stream_desc.logical_size - offset;
    const auto to_read =
        static_cast<std::uint64_t>((std::min)(static_cast<std::uint64_t>(length), remaining));
    ports::FileStreamReadRequest request;
    request.stream_index = stream_desc.stream_index;
    request.offset = offset;
    request.size = to_read;
    auto read = session.file_set->reader()->read_stream(
        request,
        std::span<std::byte>(static_cast<std::byte*>(buffer), static_cast<std::size_t>(to_read)),
        base::CancellationToken{});
    if (!read) {
        return hresult_from_error(read.error());
    }
    *bytesRead = static_cast<DWORD>(read.value());
    return S_OK;
}

HRESULT ArchiveShellModel::ReadFileData(const std::wstring& rootPath, const std::wstring& itemId,
                                        const ULONGLONG offset, void* buffer, const DWORD length,
                                        DWORD* bytesRead) {
    if (buffer == nullptr || bytesRead == nullptr) {
        return E_POINTER;
    }
    *bytesRead = 0;

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ArchiveSession& session = GetSessionLocked(rootPath);
    const HRESULT open_hr = EnsureSessionOpened(session);
    if (FAILED(open_hr)) {
        return open_hr;
    }
    if (session.content_kind == ContentKind::kFileSet) {
        if (!is_file_set_id(itemId)) {
            return E_INVALIDARG;
        }
        return ReadFileSetDataLocked(session, itemId, offset, buffer, length, bytesRead);
    }
    if (!is_ntfs_id(itemId)) {
        return E_INVALIDARG;
    }
    return ReadNtfsFileDataLocked(session, itemId, offset, buffer, length, bytesRead);
}

} // namespace aegra::shell
