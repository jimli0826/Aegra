#include "windows_file_special_folders.h"

#include "windows_file_names.h"

#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#include <objbase.h>

#include <cstring>
#include <optional>
#include <string_view>

namespace aegra::adapters::windows_filesystem::detail {
namespace {

struct SpecialFolderSpec final {
    const GUID* folder_id;
    const char* display_name;
};

// Fixed English labels match the product Source tree quick-access row (Explorer order).
// FOLDERID_* are runtime GUID constants on Windows — not usable in constexpr.
const SpecialFolderSpec kSpecialFolders[] = {
    {&FOLDERID_Desktop, "Desktop"},     {&FOLDERID_Downloads, "Downloads"},
    {&FOLDERID_Documents, "Documents"}, {&FOLDERID_Pictures, "Pictures"},
    {&FOLDERID_Music, "Music"},         {&FOLDERID_Videos, "Videos"},
};

[[nodiscard]] std::wstring known_folder_path(const GUID& folder_id) {
    PWSTR raw = nullptr;
    const HRESULT hr =
        ::SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT | KF_FLAG_DONT_VERIFY, nullptr, &raw);
    if (FAILED(hr) || raw == nullptr) {
        if (raw != nullptr) {
            ::CoTaskMemFree(raw);
        }
        return {};
    }
    std::wstring path(raw);
    ::CoTaskMemFree(raw);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

[[nodiscard]] std::wstring volume_name_for_path(const std::wstring& absolute_path) {
    if (absolute_path.empty()) {
        return {};
    }
    wchar_t mount[MAX_PATH]{};
    if (::GetVolumePathNameW(absolute_path.c_str(), mount, MAX_PATH) == FALSE) {
        return {};
    }
    wchar_t volume[MAX_PATH]{};
    if (::GetVolumeNameForVolumeMountPointW(mount, volume, MAX_PATH) == FALSE) {
        return {};
    }
    return std::wstring(volume);
}

[[nodiscard]] bool equals_ignore_case(const std::wstring_view left,
                                      const std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto a = left[index];
        auto b = right[index];
        if (a >= L'A' && a <= L'Z') {
            a = static_cast<wchar_t>(a - L'A' + L'a');
        }
        if (b >= L'A' && b <= L'Z') {
            b = static_cast<wchar_t>(b - L'A' + L'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::wstring binding_volume_name(const SnapshotVolumeBinding& root) {
    if (root.snapshot_root_utf16.empty()) {
        return {};
    }
    std::wstring path(root.snapshot_root_utf16.begin(), root.snapshot_root_utf16.end());
    if (path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    // Volume GUID roots already are volume names; drive letters need resolution.
    if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' && path[2] == L'?' &&
        path[3] == L'\\') {
        // Normalize to the form returned by GetVolumeNameForVolumeMountPointW.
        wchar_t volume[MAX_PATH]{};
        if (::GetVolumeNameForVolumeMountPointW(path.c_str(), volume, MAX_PATH) != FALSE) {
            return std::wstring(volume);
        }
        return path;
    }
    return volume_name_for_path(path);
}

[[nodiscard]] const SnapshotVolumeBinding*
find_volume_root(const std::vector<SnapshotVolumeBinding>& roots,
                 const std::wstring& volume_name) {
    if (volume_name.empty()) {
        return nullptr;
    }
    for (const auto& root : roots) {
        const auto bound = binding_volume_name(root);
        if (!bound.empty() && equals_ignore_case(bound, volume_name)) {
            return &root;
        }
        // Fallback: identity string may already be the volume GUID path.
        if (!root.volume_identity.empty()) {
            std::wstring identity(root.volume_identity.begin(), root.volume_identity.end());
            if (!identity.empty() && identity.back() != L'\\') {
                identity.push_back(L'\\');
            }
            if (equals_ignore_case(identity, volume_name)) {
                return &root;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] std::wstring strip_volume_prefix(const std::wstring& absolute_path,
                                              const std::wstring& volume_name) {
    // Prefer DOS mount point (C:\) so relative components stay user-readable.
    wchar_t mount[MAX_PATH]{};
    if (::GetVolumePathNameW(absolute_path.c_str(), mount, MAX_PATH) == FALSE) {
        return {};
    }
    const std::wstring mount_point(mount);
    if (mount_point.empty() || absolute_path.size() < mount_point.size()) {
        return {};
    }
    if (!equals_ignore_case(
            std::wstring_view(absolute_path.data(), mount_point.size()),
            std::wstring_view(mount_point))) {
        return {};
    }
    std::wstring relative = absolute_path.substr(mount_point.size());
    while (!relative.empty() && (relative.front() == L'\\' || relative.front() == L'/')) {
        relative.erase(relative.begin());
    }
    static_cast<void>(volume_name);
    return relative;
}

[[nodiscard]] std::vector<contracts::EncodedName>
split_relative_components(const std::wstring& relative) {
    std::vector<contracts::EncodedName> components;
    std::size_t begin = 0;
    while (begin < relative.size()) {
        while (begin < relative.size() &&
               (relative[begin] == L'\\' || relative[begin] == L'/')) {
            ++begin;
        }
        if (begin >= relative.size()) {
            break;
        }
        std::size_t end = begin;
        while (end < relative.size() && relative[end] != L'\\' && relative[end] != L'/') {
            ++end;
        }
        const std::wstring_view piece(relative.data() + begin, end - begin);
        if (!piece.empty() && piece != L"." && piece != L"..") {
            components.push_back(make_utf16_name(piece));
        }
        begin = end;
    }
    return components;
}

[[nodiscard]] bool directory_exists(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

} // namespace

std::vector<SpecialFolderBrowseRoot>
resolve_special_folder_browse_roots(const std::vector<SnapshotVolumeBinding>& volume_roots) {
    std::vector<SpecialFolderBrowseRoot> result;
    if (volume_roots.empty()) {
        return result;
    }
    result.reserve(sizeof(kSpecialFolders) / sizeof(kSpecialFolders[0]));
    for (const auto& spec : kSpecialFolders) {
        auto path = known_folder_path(*spec.folder_id);
        if (path.empty() || !directory_exists(path)) {
            continue;
        }
        const auto volume_name = volume_name_for_path(path);
        const auto* root = find_volume_root(volume_roots, volume_name);
        if (root == nullptr) {
            continue;
        }
        const auto relative = strip_volume_prefix(path, volume_name);
        wchar_t mount[MAX_PATH]{};
        if (::GetVolumePathNameW(path.c_str(), mount, MAX_PATH) == FALSE) {
            continue;
        }
        const std::wstring mount_point(mount);
        // Path deeper than the mount but strip failed → skip (do not select whole volume).
        if (relative.empty() && path.size() > mount_point.size()) {
            continue;
        }
        SpecialFolderBrowseRoot folder;
        folder.volume_identity = root->volume_identity;
        folder.relative_components = split_relative_components(relative);
        folder.absolute_path = std::move(path);
        folder.display_name = spec.display_name;
        result.push_back(std::move(folder));
    }
    return result;
}

} // namespace aegra::adapters::windows_filesystem::detail
