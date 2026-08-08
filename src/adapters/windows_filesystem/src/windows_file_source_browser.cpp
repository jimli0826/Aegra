#include "aegra/adapters/windows_filesystem/windows_filesystem.h"

#include "windows_file_handle.h"
#include "windows_file_names.h"

#include "aegra/base/error.h"

#include <Windows.h>

#include <charconv>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_filesystem {
namespace {

struct BrowseNode final {
    std::string volume_identity;
    std::vector<contracts::EncodedName> relative_components;
    contracts::FileEntryKind kind{contracts::FileEntryKind::kDirectory};
    std::wstring absolute_path;
    std::string display_name;
};

[[nodiscard]] std::string make_token(const std::uint64_t id) { return std::to_string(id); }

[[nodiscard]] base::Result<std::uint64_t> parse_token(const std::string& token) {
    std::uint64_t value = 0;
    const auto* begin = token.data();
    const auto* end = begin + token.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kInvalidArgument, "browse node token is invalid"});
    }
    return base::Result<std::uint64_t>::success(value);
}

/// UTF-16LE file name → UTF-8 for UI display (supports Chinese and other BMP text).
[[nodiscard]] std::string utf16_display_name(const std::wstring_view name) {
    if (name.empty()) {
        return ".";
    }
    const int required =
        ::WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0,
                              nullptr, nullptr);
    if (required <= 0) {
        return ".";
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), utf8.data(),
                              required, nullptr, nullptr) != required) {
        return ".";
    }
    // Contracts valid_text forbids C0 controls and DEL; strip them from the projection.
    std::string cleaned;
    cleaned.reserve(utf8.size());
    for (const unsigned char unit : utf8) {
        if (unit >= 0x20U && unit != 0x7FU) {
            cleaned.push_back(static_cast<char>(unit));
        }
    }
    return cleaned.empty() ? "." : cleaned;
}

[[nodiscard]] bool equals_ignore_case_ascii(const std::wstring_view left,
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

/// Hide Windows system-protected entries from the file browse tree (not from backup itself).
[[nodiscard]] bool is_system_hidden_browse_entry(const WIN32_FIND_DATAW& data) noexcept {
    const std::wstring_view name(data.cFileName);
    if (name.empty() || name == L"." || name == L"..") {
        return true;
    }
    // Explicit well-known names even if attributes differ across SKUs.
    if (equals_ignore_case_ascii(name, L"System Volume Information") ||
        equals_ignore_case_ascii(name, L"$RECYCLE.BIN") ||
        equals_ignore_case_ascii(name, L"$WinREAgent") ||
        equals_ignore_case_ascii(name, L"Recovery") ||
        equals_ignore_case_ascii(name, L"Config.Msi") ||
        equals_ignore_case_ascii(name, L"pagefile.sys") ||
        equals_ignore_case_ascii(name, L"hiberfil.sys") ||
        equals_ignore_case_ascii(name, L"swapfile.sys")) {
        return true;
    }
    constexpr DWORD kSystemHidden = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    return (data.dwFileAttributes & kSystemHidden) == kSystemHidden;
}

} // namespace

struct WindowsFileSourceBrowser::Impl final {
    std::vector<SnapshotVolumeBinding> roots;
    std::unordered_map<std::uint64_t, BrowseNode> nodes;
    std::uint64_t next_token{1};

    [[nodiscard]] std::uint64_t insert(BrowseNode node) {
        const auto id = next_token++;
        nodes.emplace(id, std::move(node));
        return id;
    }
};

WindowsFileSourceBrowser::WindowsFileSourceBrowser(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

WindowsFileSourceBrowser::~WindowsFileSourceBrowser() = default;

base::Result<std::unique_ptr<WindowsFileSourceBrowser>>
WindowsFileSourceBrowser::create(std::vector<SnapshotVolumeBinding> roots) {
    if (roots.empty()) {
        return base::Result<std::unique_ptr<WindowsFileSourceBrowser>>::failure(
            {base::ErrorCode::kInvalidArgument, "browse roots are required"});
    }
    auto implementation = std::make_unique<Impl>();
    implementation->roots = std::move(roots);
    return base::Result<std::unique_ptr<WindowsFileSourceBrowser>>::success(
        std::unique_ptr<WindowsFileSourceBrowser>(
            new WindowsFileSourceBrowser(std::move(implementation))));
}

base::Result<contracts::FileSourceNodePage>
WindowsFileSourceBrowser::list_children(const ports::FileBrowseCaller& caller,
                                        const std::optional<std::string>& parent_node_token,
                                        const contracts::ServicePageRequest& page,
                                        const bool include_unavailable,
                                        const base::CancellationToken cancellation) {
    static_cast<void>(caller);
    static_cast<void>(include_unavailable);
    if (cancellation.stop_requested()) {
        return base::Result<contracts::FileSourceNodePage>::failure(
            {base::ErrorCode::kCancelled, "browse cancelled"});
    }
    if (page.maximum_results == 0) {
        return base::Result<contracts::FileSourceNodePage>::failure(
            {base::ErrorCode::kInvalidArgument, "browse page size must be positive"});
    }
    contracts::FileSourceNodePage result;
    std::wstring parent_path;
    std::string volume_identity;
    std::vector<contracts::EncodedName> parent_components;
    if (!parent_node_token.has_value()) {
        for (const auto& root : implementation_->roots) {
            BrowseNode node;
            node.volume_identity = root.volume_identity;
            node.kind = contracts::FileEntryKind::kDirectory;
            node.absolute_path.assign(root.snapshot_root_utf16.begin(),
                                      root.snapshot_root_utf16.end());
            node.display_name =
                root.display_name.empty() ? root.volume_identity : root.display_name;
            const auto id = implementation_->insert(std::move(node));
            contracts::FileSourceNode summary;
            summary.node_token = make_token(id);
            summary.display_name = implementation_->nodes[id].display_name;
            summary.entry_kind = contracts::FileEntryKind::kDirectory;
            summary.selectability = contracts::FileNodeSelectability::kSelectable;
            summary.has_children = true;
            summary.is_directory = true;
            summary.availability = contracts::SourceAvailability::kAvailable;
            result.items.push_back(std::move(summary));
            if (result.items.size() >= page.maximum_results) {
                break;
            }
        }
        return base::Result<contracts::FileSourceNodePage>::success(std::move(result));
    }
    auto parent_id = parse_token(*parent_node_token);
    if (!parent_id) {
        return base::Result<contracts::FileSourceNodePage>::failure(parent_id.error());
    }
    const auto parent = implementation_->nodes.find(parent_id.value());
    if (parent == implementation_->nodes.end()) {
        return base::Result<contracts::FileSourceNodePage>::failure(
            {base::ErrorCode::kNotFound, "browse parent token was not found"});
    }
    parent_path = parent->second.absolute_path;
    volume_identity = parent->second.volume_identity;
    parent_components = parent->second.relative_components;
    const auto pattern = parent_path + L"\\*";
    WIN32_FIND_DATAW data{};
    const HANDLE find =
        FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        return base::Result<contracts::FileSourceNodePage>::failure(
            detail::win32_error(GetLastError(), "FindFirstFileExW"));
    }
    std::uint32_t skipped = 0;
    std::uint32_t start = 0;
    if (page.continuation_token.has_value()) {
        auto parsed = parse_token(*page.continuation_token);
        if (parsed) {
            start = static_cast<std::uint32_t>(parsed.value());
        }
    }
    do {
        if (is_system_hidden_browse_entry(data)) {
            continue;
        }
        if (skipped < start) {
            ++skipped;
            continue;
        }
        if (result.items.size() >= page.maximum_results) {
            result.continuation_token = make_token(skipped);
            break;
        }
        const std::wstring_view name(data.cFileName);
        BrowseNode node;
        node.volume_identity = volume_identity;
        node.relative_components = parent_components;
        node.relative_components.push_back(detail::make_utf16_name(name));
        const bool is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool is_reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        const bool is_sparse = (data.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
        node.kind = is_directory ? contracts::FileEntryKind::kDirectory
                                 : contracts::FileEntryKind::kFile;
        node.absolute_path = parent_path + L'\\' + data.cFileName;
        node.display_name = utf16_display_name(name);
        const auto id = implementation_->insert(std::move(node));
        contracts::FileSourceNode summary;
        summary.node_token = make_token(id);
        summary.display_name = implementation_->nodes[id].display_name;
        summary.entry_kind = implementation_->nodes[id].kind;
        // FI0 browse: reparse/sparse are not selectable; hard-link/ADS caught at backup enum.
        if (is_reparse || is_sparse) {
            summary.selectability = contracts::FileNodeSelectability::kUnsupported;
            summary.message_code = is_reparse ? "file_source.unsupported_reparse"
                                              : "file_source.unsupported_sparse";
        } else {
            summary.selectability = contracts::FileNodeSelectability::kSelectable;
        }
        summary.has_children =
            summary.entry_kind == contracts::FileEntryKind::kDirectory && !is_reparse;
        summary.is_directory = summary.entry_kind == contracts::FileEntryKind::kDirectory;
        summary.availability = contracts::SourceAvailability::kAvailable;
        result.items.push_back(std::move(summary));
        ++skipped;
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
    return base::Result<contracts::FileSourceNodePage>::success(std::move(result));
}

base::Result<contracts::FileSourceRef>
WindowsFileSourceBrowser::resolve_selection(const ports::FileBrowseCaller& caller,
                                            const std::string& node_token,
                                            const contracts::FileRecursion recursion,
                                            const std::string& display_label,
                                            const base::CancellationToken cancellation) {
    static_cast<void>(caller);
    if (cancellation.stop_requested()) {
        return base::Result<contracts::FileSourceRef>::failure(
            {base::ErrorCode::kCancelled, "resolve selection cancelled"});
    }
    auto id = parse_token(node_token);
    if (!id) {
        return base::Result<contracts::FileSourceRef>::failure(id.error());
    }
    const auto node = implementation_->nodes.find(id.value());
    if (node == implementation_->nodes.end()) {
        return base::Result<contracts::FileSourceRef>::failure(
            {base::ErrorCode::kNotFound, "browse node token was not found"});
    }
    contracts::FileSourceRef ref;
    // selection_id is assigned by Service (canonical UUID) before durable persistence.
    ref.selection_id = "00000000-0000-4000-8000-000000000000";
    ref.volume_identity = node->second.volume_identity;
    ref.relative_components = node->second.relative_components;
    ref.entry_kind = node->second.kind;
    ref.recursion = recursion;
    ref.display_label = display_label.empty() ? node->second.display_name : display_label;
    // Temporary placeholder UUID so contracts validate shape; Service replaces it.
    auto validated = contracts::validate_file_source_ref(ref);
    if (!validated) {
        return base::Result<contracts::FileSourceRef>::failure(validated.error());
    }
    ref.selection_id.clear();
    return base::Result<contracts::FileSourceRef>::success(std::move(ref));
}

} // namespace aegra::adapters::windows_filesystem
