#include "aegra/adapters/dokan/disk_mount.h"

#include "cow_backing_store.h"
#include "dokan_file_system.h"
#include "dokan_ntstatus.h"
#include "file_set_fs.h"
#include "partition_filter.h"
#include "vhdx_disk_image.h"
#include "virt_disk_attach.h"

#include <dokan/dokan.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace aegra::adapters::dokan {
namespace {

namespace fs = std::filesystem;
using detail::CowBackingStore;
using detail::DokanFileSystem;
using detail::FileSetFileSystem;
using detail::VirtualDiskEntry;
using detail::VhdxDiskImage;

constexpr const char* kMsgDokanUnavailable = "mount.dokan_unavailable";
constexpr const char* kMsgDiskNotFound = "mount.disk_not_found";
constexpr const char* kMsgNoDataPartition = "mount.no_data_partition";
constexpr const char* kMsgAlreadyMounted = "mount.already_mounted";
constexpr const char* kMsgDokanFailed = "mount.dokan_failed";
constexpr const char* kMsgAttachFailed = "mount.attach_failed";
constexpr const char* kMsgUnmountFailed = "mount.unmount_failed";
constexpr const char* kMsgInvalidArgument = "mount.invalid_argument";
constexpr const char* kMsgNoFreeDriveLetter = "mount.no_free_drive_letter";

struct ActiveMountSession {
    MountSessionInfo info;
    std::uint32_t source_disk_number{0};
    // Session working root (overlay_dir from mount_host). Holds mnt/ + overlay sidecars.
    std::string session_root;
    std::string dokan_mount_point;
    std::wstring vhdx_path;
    std::wstring overlay_base;
    std::unique_ptr<DokanFileSystem> fs;
    // file_set sessions: drive-letter Dokan namespace, no VHD/overlay involved.
    std::unique_ptr<FileSetFileSystem> file_fs;
    HANDLE vhd_handle{INVALID_HANDLE_VALUE};
};

std::mutex g_sessions_mu;
std::map<std::string, std::unique_ptr<ActiveMountSession>> g_sessions;

base::Error make_error(base::ErrorCode code, std::string_view message) {
    return base::Error{code, std::string(message)};
}

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                            nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), needed);
    return out;
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                            nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

bool is_valid_preferred_letter(std::string_view preferred) {
    if (preferred.empty()) {
        return true;
    }
    if (preferred.size() == 1) {
        const char c =
            static_cast<char>(std::toupper(static_cast<unsigned char>(preferred[0])));
        return c >= 'D' && c <= 'Z';
    }
    if (preferred.size() == 2 && preferred[1] == ':') {
        const char c =
            static_cast<char>(std::toupper(static_cast<unsigned char>(preferred[0])));
        return c >= 'D' && c <= 'Z';
    }
    return false;
}

std::string normalize_path_utf8(const fs::path& path) {
    std::string out = wide_to_utf8(path.lexically_normal().wstring());
    while (!out.empty() && (out.back() == '\\' || out.back() == '/')) {
        out.pop_back();
    }
    return out;
}

// Dokan directory mounts require an *empty* NTFS folder. Overlay sidecars must
// live outside that folder (same layout as the old backup mount host).
bool prepare_session_directories(const fs::path& session_root, std::string& out_session_utf8,
                                 std::string& out_mnt_utf8) {
    std::error_code ec;
    fs::create_directories(session_root, ec);
    if (ec) {
        return false;
    }

    const fs::path mnt = session_root / "mnt";
    // Recreate empty mount point so leftover overlay/temp files cannot fail Dokan.
    fs::remove_all(mnt, ec);
    ec.clear();
    fs::create_directories(mnt, ec);
    if (ec) {
        return false;
    }

    out_session_utf8 = normalize_path_utf8(session_root);
    out_mnt_utf8 = normalize_path_utf8(mnt);
    return !out_session_utf8.empty() && !out_mnt_utf8.empty();
}

void cleanup_overlay_files(const std::wstring& overlay_base) {
    if (overlay_base.empty()) {
        return;
    }
    DeleteFileW((overlay_base + L".overlay").c_str());
    DeleteFileW((overlay_base + L".overlay.map").c_str());
}

void remove_directory_tree(const std::string& path_utf8) {
    if (path_utf8.empty()) {
        return;
    }
    std::error_code ec;
    for (int i = 0; i < 5; ++i) {
        ec.clear();
        fs::remove_all(path_utf8, ec);
        if (!ec) {
            break;
        }
        Sleep(200);
    }
}

void tear_down_session(ActiveMountSession& session) {
    // 1) Detach VHD first so vhdmp releases handles on the Dokan VHDX.
    if (session.vhd_handle != INVALID_HANDLE_VALUE &&
        session.vhd_handle != nullptr) {
        (void)detail::detach_vhd_handle(session.vhd_handle);
        session.vhd_handle = INVALID_HANDLE_VALUE;
    }

    // 2) Unmount Dokan.
    if (session.fs) {
        session.fs->close(15000);
        session.fs.reset();
    }
    if (session.file_fs) {
        session.file_fs->close(15000);
        session.file_fs.reset();
    }

    cleanup_overlay_files(session.overlay_base);

    // 3) Remove temporary session root (mnt/ + any leftover sidecars).
    if (!session.session_root.empty()) {
        remove_directory_tree(session.session_root);
    } else if (!session.dokan_mount_point.empty()) {
        remove_directory_tree(session.dokan_mount_point);
    }
}

bool session_exists_locked(std::string_view session_id) {
    return g_sessions.find(std::string(session_id)) != g_sessions.end();
}

base::Result<std::unique_ptr<DokanFileSystem>>
create_dokan_vhdx(ports::IRandomAccessReader& reader, std::uint32_t disk_number,
                  const std::wstring& mount_dir, const std::wstring& overlay_base,
                  std::wstring& out_vhdx_path) {
    const std::wstring leaf =
        L"disk" + std::to_wstring(disk_number) + L".vhdx";

    // Match old backup host: COW sidecars are *outside* the Dokan mount folder.
    // Putting them under mount_dir makes the folder non-empty and Dokan fails
    // with DOKAN_MOUNT_POINT_ERROR (-6) → mount.dokan_failed.
    auto backing = std::make_unique<CowBackingStore>();
    if (!backing->open_reader(&reader, overlay_base, /*read_only=*/true)) {
        return base::Result<std::unique_ptr<DokanFileSystem>>::failure(
            make_error(base::ErrorCode::kIoFailure, kMsgDokanFailed));
    }

    auto disk = std::make_unique<VhdxDiskImage>(*backing);
    disk->rebuild();

    VirtualDiskEntry entry;
    entry.leaf_name = leaf;
    entry.reader = &reader;
    entry.backing = std::move(backing);
    entry.disk = std::move(disk);
    entry.overlay_base = overlay_base;

    std::vector<VirtualDiskEntry> entries;
    entries.push_back(std::move(entry));

    auto fs = std::make_unique<DokanFileSystem>(std::move(entries), /*read_only=*/true);
    const int status = fs->mount(mount_dir);
    if (status != DOKAN_SUCCESS) {
        cleanup_overlay_files(overlay_base);
        return base::Result<std::unique_ptr<DokanFileSystem>>::failure(
            make_error(base::ErrorCode::kIoFailure, kMsgDokanFailed));
    }

    out_vhdx_path = mount_dir;
    if (!out_vhdx_path.empty() && out_vhdx_path.back() != L'\\' &&
        out_vhdx_path.back() != L'/') {
        out_vhdx_path += L'\\';
    }
    out_vhdx_path += leaf;
    return base::Result<std::unique_ptr<DokanFileSystem>>::success(std::move(fs));
}

} // namespace

bool is_dokan_available() noexcept {
    return DokanDriverVersion() != 0;
}

base::Result<MountSessionInfo>
mount_whole_disk_readonly(ports::IRandomAccessReader& reader,
                          const format::Manifest& manifest,
                          std::uint32_t source_disk_number,
                          std::string_view preferred_drive_letter,
                          const std::filesystem::path& overlay_root,
                          std::string_view session_id) {
    MountSessionInfo info;
    info.source_disk_number = source_disk_number;
    info.session_id = std::string(session_id);

    if (session_id.empty() || overlay_root.empty() ||
        !is_valid_preferred_letter(preferred_drive_letter)) {
        info.message_code = kMsgInvalidArgument;
        return base::Result<MountSessionInfo>::failure(
            make_error(base::ErrorCode::kInvalidArgument, kMsgInvalidArgument));
    }

    if (!is_dokan_available()) {
        info.message_code = kMsgDokanUnavailable;
        return base::Result<MountSessionInfo>::failure(
            make_error(base::ErrorCode::kIoFailure, kMsgDokanUnavailable));
    }

    if (!detail::manifest_has_disk(manifest, source_disk_number)) {
        info.message_code = kMsgDiskNotFound;
        return base::Result<MountSessionInfo>::failure(
            make_error(base::ErrorCode::kNotFound, kMsgDiskNotFound));
    }

    {
        std::lock_guard lock(g_sessions_mu);
        if (session_exists_locked(session_id)) {
            info.message_code = kMsgAlreadyMounted;
            return base::Result<MountSessionInfo>::failure(
                make_error(base::ErrorCode::kConflict, kMsgAlreadyMounted));
        }
    }

    const auto data_parts =
        detail::find_data_partition_numbers(manifest, source_disk_number);
    if (data_parts.empty()) {
        info.message_code = kMsgNoDataPartition;
        return base::Result<MountSessionInfo>::failure(
            make_error(base::ErrorCode::kNotFound, kMsgNoDataPartition));
    }

    // overlay_root is already session-scoped (Service sends
    // <data_dir>/mount_overlays/<session_id>). Do not nest session_id again.
    const fs::path session_root = overlay_root;
    std::string session_root_utf8;
    std::string dokan_mount_utf8;
    if (!prepare_session_directories(session_root, session_root_utf8,
                                     dokan_mount_utf8)) {
        info.message_code = kMsgInvalidArgument;
        return base::Result<MountSessionInfo>::failure(
            make_error(base::ErrorCode::kIoFailure, kMsgInvalidArgument));
    }

    const std::wstring mount_dir_w = utf8_to_wide(dokan_mount_utf8);
    const std::wstring overlay_base =
        utf8_to_wide(session_root_utf8) + L"\\disk" +
        std::to_wstring(source_disk_number) + L".vhdx";
    std::wstring vhdx_path;
    auto fs_result =
        create_dokan_vhdx(reader, source_disk_number, mount_dir_w, overlay_base,
                          vhdx_path);
    if (!fs_result) {
        cleanup_overlay_files(overlay_base);
        remove_directory_tree(session_root_utf8);
        info.message_code = kMsgDokanFailed;
        return base::Result<MountSessionInfo>::failure(fs_result.error());
    }

    Sleep(200);

    detail::VhdAttachResult attach;
    const std::string preferred(preferred_drive_letter);
    if (!detail::attach_vhdx_readonly(vhdx_path, data_parts, preferred, attach)) {
        fs_result.value()->close(15000);
        cleanup_overlay_files(overlay_base);
        remove_directory_tree(session_root_utf8);
        info.message_code = kMsgAttachFailed;
        return base::Result<MountSessionInfo>::failure(
            make_error(base::ErrorCode::kIoFailure, kMsgAttachFailed));
    }

    auto session = std::make_unique<ActiveMountSession>();
    session->source_disk_number = source_disk_number;
    session->session_root = session_root_utf8;
    session->dokan_mount_point = dokan_mount_utf8;
    session->vhdx_path = vhdx_path;
    session->overlay_base = overlay_base;
    session->fs = std::move(fs_result.value());
    session->vhd_handle = attach.vhd_handle;

    info.drive_letters = attach.drive_letters;
    // UI "Drive(s)" column: space-separated letters (old MountPage driveLettersText).
    {
        std::string joined;
        for (const auto& letter : attach.drive_letters) {
            if (letter.empty()) {
                continue;
            }
            if (!joined.empty()) {
                joined.push_back(' ');
            }
            joined += letter;
        }
        info.mount_point = std::move(joined);
    }
    info.device_number = attach.windows_disk_number >= 0
                             ? static_cast<std::uint32_t>(attach.windows_disk_number)
                             : 0xFFFFFFFFu;
    info.disk_size_bytes = attach.total_data_size;
    info.message_code.clear();
    session->info = info;

    {
        std::lock_guard lock(g_sessions_mu);
        if (session_exists_locked(session_id)) {
            tear_down_session(*session);
            info.message_code = kMsgAlreadyMounted;
            return base::Result<MountSessionInfo>::failure(
                make_error(base::ErrorCode::kConflict, kMsgAlreadyMounted));
        }
        g_sessions[std::string(session_id)] = std::move(session);
    }

    return base::Result<MountSessionInfo>::success(std::move(info));
}

base::Result<MountSessionInfo>
mount_file_set_readonly(ports::IFileRecoveryPointReader& reader,
                        std::string_view preferred_drive_letter,
                        std::string_view session_id) {
    MountSessionInfo info;
    info.session_id = std::string(session_id);

    if (session_id.empty() || !is_valid_preferred_letter(preferred_drive_letter)) {
        info.message_code = kMsgInvalidArgument;
        return base::Result<MountSessionInfo>::failure(
            make_error(base::ErrorCode::kInvalidArgument, kMsgInvalidArgument));
    }

    if (!is_dokan_available()) {
        info.message_code = kMsgDokanUnavailable;
        return base::Result<MountSessionInfo>::failure(
            make_error(base::ErrorCode::kIoFailure, kMsgDokanUnavailable));
    }

    {
        std::lock_guard lock(g_sessions_mu);
        if (session_exists_locked(session_id)) {
            info.message_code = kMsgAlreadyMounted;
            return base::Result<MountSessionInfo>::failure(
                make_error(base::ErrorCode::kConflict, kMsgAlreadyMounted));
        }
    }

    std::string letter =
        detail::normalize_drive_letter(std::string(preferred_drive_letter));
    if (letter.empty() || detail::drive_letter_exists(letter)) {
        letter = detail::find_free_drive_letter();
    }
    if (letter.empty()) {
        info.message_code = kMsgNoFreeDriveLetter;
        return base::Result<MountSessionInfo>::failure(
            make_error(base::ErrorCode::kConflict, kMsgNoFreeDriveLetter));
    }

    auto fs = std::make_unique<FileSetFileSystem>(reader);
    const std::wstring mount_point =
        std::wstring(1, static_cast<wchar_t>(letter[0])) + L":\\";
    if (fs->mount(mount_point) != DOKAN_SUCCESS) {
        info.message_code = kMsgDokanFailed;
        return base::Result<MountSessionInfo>::failure(
            make_error(base::ErrorCode::kIoFailure, kMsgDokanFailed));
    }

    // The mount manager may adjust the letter; wait for the Mounted callback.
    for (int i = 0; i < 25 && fs->actual_mount_point().empty(); ++i) {
        Sleep(200);
    }
    if (!fs->is_running()) {
        fs->close(15000);
        info.message_code = kMsgDokanFailed;
        return base::Result<MountSessionInfo>::failure(
            make_error(base::ErrorCode::kIoFailure, kMsgDokanFailed));
    }
    const auto& actual = fs->actual_mount_point();
    if (!actual.empty() && iswalpha(actual[0]) != 0) {
        letter = std::string(1, static_cast<char>(towupper(actual[0]))) + ":";
    }

    auto session = std::make_unique<ActiveMountSession>();
    session->file_fs = std::move(fs);

    info.drive_letters = {letter};
    info.mount_point = letter;
    info.device_number = 0xFFFFFFFFu;
    info.disk_size_bytes = 0;
    info.message_code.clear();
    session->info = info;

    {
        std::lock_guard lock(g_sessions_mu);
        if (session_exists_locked(session_id)) {
            tear_down_session(*session);
            info.message_code = kMsgAlreadyMounted;
            return base::Result<MountSessionInfo>::failure(
                make_error(base::ErrorCode::kConflict, kMsgAlreadyMounted));
        }
        g_sessions[std::string(session_id)] = std::move(session);
    }

    return base::Result<MountSessionInfo>::success(std::move(info));
}

base::Result<void> unmount_session(std::string_view session_id) {
    if (session_id.empty()) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kInvalidArgument, kMsgInvalidArgument));
    }

    std::unique_ptr<ActiveMountSession> session;
    {
        std::lock_guard lock(g_sessions_mu);
        auto it = g_sessions.find(std::string(session_id));
        if (it == g_sessions.end()) {
            return base::Result<void>::failure(
                make_error(base::ErrorCode::kNotFound, kMsgUnmountFailed));
        }
        session = std::move(it->second);
        g_sessions.erase(it);
    }

    tear_down_session(*session);
    return base::Result<void>::success();
}

base::Result<void> unmount_all_sessions() {
    std::vector<std::string> ids;
    {
        std::lock_guard lock(g_sessions_mu);
        ids.reserve(g_sessions.size());
        for (const auto& kv : g_sessions) {
            ids.push_back(kv.first);
        }
    }
    for (const auto& id : ids) {
        (void)unmount_session(id);
    }
    return base::Result<void>::success();
}

std::vector<MountSessionInfo> list_sessions() {
    std::vector<MountSessionInfo> list;
    std::lock_guard lock(g_sessions_mu);
    list.reserve(g_sessions.size());
    for (const auto& kv : g_sessions) {
        if (kv.second) {
            list.push_back(kv.second->info);
        }
    }
    return list;
}

} // namespace aegra::adapters::dokan
