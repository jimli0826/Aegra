#include "pch.h"

#include "archive_chain_resolver.h"

#include "aegra/adapters/storage_local/local_object_storage.h"
#include "aegra/base/error.h"
#include "aegra/base/uuid.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/personal_repository/chain_graph.h"

#include <cwctype>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::shell {
namespace {

namespace fs = std::filesystem;
namespace storage_api = aegra::adapters::storage_local;
namespace repo = aegra::personal_repository;

[[nodiscard]] base::Error parent_missing_error(std::string detail) {
    return {base::ErrorCode::kNotFound, std::move(detail)};
}

[[nodiscard]] std::wstring path_key(const fs::path& path) {
    std::error_code error;
    auto absolute = fs::weakly_canonical(path, error);
    if (error) {
        absolute = fs::absolute(path, error);
        if (error) {
            absolute = path;
        }
    }
    auto generic = absolute.generic_wstring();
    for (auto& ch : generic) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return generic;
}

[[nodiscard]] bool paths_equal(const fs::path& left, const fs::path& right) {
    return path_key(left) == path_key(right);
}

[[nodiscard]] base::Result<std::string> wide_path_to_utf8(const fs::path& path) {
    const auto wide = path.wstring();
    if (wide.empty()) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "repository path is empty"});
    }
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                             static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (length <= 0) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "repository path is not valid UTF-8"});
    }
    std::string utf8(static_cast<std::size_t>(length), '\0');
    const int written =
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                              length, nullptr, nullptr);
    if (written != length) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "repository path encoding failed"});
    }
    return base::Result<std::string>::success(std::move(utf8));
}

[[nodiscard]] fs::path join_object_key(const fs::path& root, const std::string& key) {
    fs::path result = root;
    std::string_view remaining = key;
    while (!remaining.empty()) {
        const auto slash = remaining.find('/');
        const auto component =
            slash == std::string_view::npos ? remaining : remaining.substr(0, slash);
        if (!component.empty() && component != "." && component != "..") {
            const auto* begin = reinterpret_cast<const char8_t*>(component.data());
            result /= fs::path(std::u8string(begin, begin + component.size()));
        }
        if (slash == std::string_view::npos) {
            break;
        }
        remaining = remaining.substr(slash + 1);
    }
    return result;
}

[[nodiscard]] bool object_exists(const fs::path& path) noexcept {
    std::error_code error;
    return fs::is_regular_file(path, error) && !error;
}

[[nodiscard]] base::Result<fs::path>
find_repository_root(const fs::path& tip_archive_path) {
    std::error_code error;
    auto current = tip_archive_path.parent_path();
    if (current.empty()) {
        return base::Result<fs::path>::failure(
            parent_missing_error("shell.parent_missing: archive has no parent directory"));
    }
    for (std::uint32_t depth = 0; depth < kMaximumRepositoryAncestorWalk; ++depth) {
        const auto marker = current / L"aegra.repository";
        if (object_exists(marker)) {
            return base::Result<fs::path>::success(current);
        }
        const auto parent = current.parent_path();
        if (parent.empty() || paths_equal(parent, current)) {
            break;
        }
        current = parent;
    }
    return base::Result<fs::path>::failure(
        parent_missing_error("shell.parent_missing: aegra.repository not found"));
}

[[nodiscard]] base::Result<ResolvedArchiveChain>
resolve_from_catalog(const fs::path& repository_root, const fs::path& tip_archive_path,
                     const std::string& tip_uuid) {
    auto locator = wide_path_to_utf8(repository_root);
    if (!locator) {
        return base::Result<ResolvedArchiveChain>::failure(locator.error());
    }

    storage_api::LocalRepositoryStorageFactory factory;
    auto storage = factory.open(locator.value(), base::CancellationToken{});
    if (!storage) {
        return base::Result<ResolvedArchiveChain>::failure(
            parent_missing_error("shell.parent_missing: repository storage open failed"));
    }

    repo::CatalogScannerLimits limits;
    limits.maximum_catalog_objects = 10'000;
    repo::RepositoryCatalogScanner scanner(storage.value()->reader(),
                                           storage.value()->enumerator(), limits);
    auto loaded = scanner.load_entries(base::CancellationToken{});
    if (!loaded) {
        return base::Result<ResolvedArchiveChain>::failure(
            parent_missing_error("shell.parent_missing: catalog load failed"));
    }

    const repo::CatalogEntry* tip_entry = nullptr;
    for (const auto& entry : loaded.value().entries) {
        if (entry.file_uuid == tip_uuid) {
            tip_entry = &entry;
            break;
        }
    }
    if (tip_entry == nullptr) {
        return base::Result<ResolvedArchiveChain>::failure(
            parent_missing_error("shell.parent_missing: tip not in catalog"));
    }

    const auto expected_tip_path = join_object_key(repository_root, tip_entry->archive_main_key);
    if (!paths_equal(expected_tip_path, tip_archive_path)) {
        return base::Result<ResolvedArchiveChain>::failure(parent_missing_error(
            "shell.parent_missing: archive_main_key does not match opened path"));
    }

    auto graph = repo::RecoveryPointGraph::build(std::move(loaded).value().entries);
    if (!graph) {
        return base::Result<ResolvedArchiveChain>::failure(
            parent_missing_error("shell.parent_missing: recovery point graph invalid"));
    }
    auto chain = graph.value().resolve_chain(tip_uuid);
    if (!chain) {
        return base::Result<ResolvedArchiveChain>::failure(
            parent_missing_error("shell.parent_missing: incomplete recovery point chain"));
    }

    ResolvedArchiveChain resolved;
    resolved.tip_file_uuid = tip_uuid;
    resolved.layer_paths.reserve(chain.value().size());
    for (const auto& layer : chain.value()) {
        const auto layer_path = join_object_key(repository_root, layer.archive_main_key);
        if (!object_exists(layer_path)) {
            return base::Result<ResolvedArchiveChain>::failure(
                parent_missing_error("shell.parent_missing: chain layer file missing"));
        }
        resolved.layer_paths.push_back(layer_path);
    }
    if (resolved.layer_paths.empty() ||
        !paths_equal(resolved.layer_paths.back(), tip_archive_path)) {
        return base::Result<ResolvedArchiveChain>::failure(
            parent_missing_error("shell.parent_missing: chain tip path mismatch"));
    }
    return base::Result<ResolvedArchiveChain>::success(std::move(resolved));
}

} // namespace

base::Result<ResolvedArchiveChain>
resolve_managed_archive_chain(const fs::path& tip_archive_path,
                              const std::array<std::byte, 16>& tip_file_uuid) {
    if (tip_archive_path.empty()) {
        return base::Result<ResolvedArchiveChain>::failure(
            {base::ErrorCode::kInvalidArgument, "archive path is empty"});
    }
    const auto tip_uuid = base::format_uuid(tip_file_uuid);
    if (!base::is_canonical_uuid(tip_uuid)) {
        return base::Result<ResolvedArchiveChain>::failure(
            {base::ErrorCode::kCorruptData, "archive file_uuid is invalid"});
    }

    auto repository_root = find_repository_root(tip_archive_path);
    if (!repository_root) {
        return base::Result<ResolvedArchiveChain>::failure(repository_root.error());
    }
    return resolve_from_catalog(repository_root.value(), tip_archive_path, tip_uuid);
}

} // namespace aegra::shell
