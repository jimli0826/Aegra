#include "file_recovery_point_query.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/contracts/file_set.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/ports/credential.h"
#include "aegra/ports/file_recovery_point.h"
#include "aegra/ports/repository_storage.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

constexpr std::string_view kMessageCredentialRequired = "file_recover.credential_required";
constexpr std::string_view kMessageCredentialFailed = "file_recover.credential_failed";
constexpr std::string_view kMessageCorrupt = "file_recover.corrupt";
constexpr std::string_view kMessageCatalogOnly = "file_recover.catalog_only";
constexpr std::string_view kMessageContentKind = "service.content_kind_mismatch";
constexpr std::string_view kMessageTokenInvalid = "file_recover.token_invalid";
constexpr std::size_t kMaximumDisplayNameBytes = 256;

[[nodiscard]] base::Result<std::filesystem::path> path_from_utf8(const std::string_view value) {
    try {
        const auto* begin = reinterpret_cast<const char8_t*>(value.data());
        std::filesystem::path path(std::u8string(begin, begin + value.size()));
        if (!path.is_absolute()) {
            return base::Result<std::filesystem::path>::failure(
                {base::ErrorCode::kInvalidArgument, "repository locator must be absolute"});
        }
        return base::Result<std::filesystem::path>::success(std::move(path));
    } catch (const std::exception&) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kInvalidArgument, "repository locator is invalid UTF-8"});
    }
}

[[nodiscard]] base::Result<std::filesystem::path>
resolve_archive_path(const std::string& locator, const std::string& archive_main_key) {
    auto root = path_from_utf8(locator);
    if (!root) {
        return base::Result<std::filesystem::path>::failure(root.error());
    }
    if (!archive_main_key.starts_with("archives/") ||
        archive_main_key.find('\\') != std::string::npos ||
        archive_main_key.find(':') != std::string::npos ||
        archive_main_key.find("..") != std::string::npos) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kInvalidArgument, "archive key is outside the archive root"});
    }
    std::filesystem::path relative;
    try {
        relative = std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(archive_main_key.data()), archive_main_key.size()));
    } catch (const std::exception&) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kInvalidArgument, "archive key is invalid"});
    }
    std::error_code error_code;
    const auto canonical_root = std::filesystem::weakly_canonical(root.value(), error_code);
    if (error_code) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kIoFailure, "repository root cannot be resolved"});
    }
    const auto canonical_archive =
        std::filesystem::weakly_canonical(canonical_root / relative, error_code);
    if (error_code) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kIoFailure, "archive path cannot be resolved"});
    }
    return base::Result<std::filesystem::path>::success(canonical_archive);
}

[[nodiscard]] base::Result<personal_repository::CatalogEntry>
find_catalog_entry(ports::IRepositoryStorageAccess& storage, const std::string_view recovery_point_id,
                   const base::CancellationToken cancellation) {
    personal_repository::RepositoryCatalogScanner scanner(storage.reader(), storage.enumerator(),
                                                          {});
    std::optional<std::string> token;
    for (;;) {
        personal_repository::CatalogScanRequest request;
        request.continuation_token = token;
        request.maximum_results = 100;
        auto page = scanner.scan(request, cancellation);
        if (!page) {
            return base::Result<personal_repository::CatalogEntry>::failure(page.error());
        }
        for (const auto& point : page.value().recovery_points) {
            if (point.entry.file_uuid == recovery_point_id) {
                return base::Result<personal_repository::CatalogEntry>::success(point.entry);
            }
        }
        if (!page.value().continuation_token) {
            break;
        }
        token = std::move(page.value().continuation_token);
    }
    return base::Result<personal_repository::CatalogEntry>::failure(
        {base::ErrorCode::kNotFound, "recovery point was not found in the catalog"});
}

[[nodiscard]] base::Result<std::uint64_t> parse_u64_text(const std::string_view text) {
    if (text.empty() || text.size() > 20) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kInvalidArgument, "entry id is invalid"});
    }
    std::uint64_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kInvalidArgument, "entry id is invalid"});
    }
    return base::Result<std::uint64_t>::success(value);
}

/// UTF-16LE EncodedName → UTF-8 display label (Chinese and other non-ASCII names).
[[nodiscard]] std::string project_display_name(const contracts::EncodedName& name) {
    if (name.bytes.empty() || (name.bytes.size() % 2U) != 0U) {
        return ".";
    }
    const auto unit_count = name.bytes.size() / 2U;
    std::wstring wide(unit_count, L'\0');
    std::memcpy(wide.data(), name.bytes.data(), name.bytes.size());
    const int required =
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                              nullptr, nullptr);
    if (required <= 0) {
        return ".";
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                              required, nullptr, nullptr) != required) {
        return ".";
    }
    std::string projected;
    projected.reserve(utf8.size());
    for (const unsigned char unit : utf8) {
        if (unit >= 0x20U && unit != 0x7FU) {
            projected.push_back(static_cast<char>(unit));
        }
    }
    if (projected.empty()) {
        return ".";
    }
    if (projected.size() > kMaximumDisplayNameBytes) {
        // Avoid cutting mid UTF-8 sequence: back up while first dropped byte is a trail.
        auto limit = kMaximumDisplayNameBytes;
        while (limit > 0U && limit < projected.size() &&
               (static_cast<unsigned char>(projected[limit]) & 0xC0U) == 0x80U) {
            --limit;
        }
        projected.resize(limit);
        if (projected.empty()) {
            return ".";
        }
    }
    return projected;
}

[[nodiscard]] bool connection_allows_default_credential(
    const ports::RepositoryConnectionRecord& connection) noexcept {
    return std::find(connection.capabilities.begin(), connection.capabilities.end(),
                     "archive.default_credential") != connection.capabilities.end();
}

[[nodiscard]] base::Result<std::string>
resolve_secret_material(const contracts::SecretRef& reference,
                        const base::CancellationToken cancellation) {
    adapters::windows_system::WindowsCredentialResolver resolver;
    auto secret = resolver.resolve(reference, cancellation);
    if (!secret || secret.value() == nullptr) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kUnauthorized, std::string(kMessageCredentialFailed)});
    }
    return base::Result<std::string>::success(std::string(secret.value()->view()));
}

[[nodiscard]] base::Error map_open_error(const base::Error& error, const bool credential_supplied) {
    if (error.code == base::ErrorCode::kUnauthorized) {
        return {base::ErrorCode::kUnauthorized,
                std::string(credential_supplied ? kMessageCredentialFailed
                                                : kMessageCredentialRequired)};
    }
    if (error.code == base::ErrorCode::kCorruptData ||
        error.code == base::ErrorCode::kUnsupportedVersion) {
        return {base::ErrorCode::kCorruptData, std::string(kMessageCorrupt)};
    }
    if (error.code == base::ErrorCode::kInvalidArgument &&
        error.message.find("content kind") != std::string::npos) {
        return {base::ErrorCode::kInvalidArgument, std::string(kMessageContentKind)};
    }
    return error;
}

[[nodiscard]] base::Result<std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>
open_file_recovery_reader(const std::filesystem::path& archive_path,
                          const contracts::ListRecoveryPointEntriesRequest& request,
                          const ports::RepositoryConnectionRecord& connection,
                          const base::CancellationToken cancellation) {
    adapters::personal_archive::ArchiveOpenRequest open_request;
    open_request.source = archive_path;
    if (request.archive_secret_ref && !request.archive_secret_ref->empty()) {
        contracts::SecretRef reference;
        reference.value = *request.archive_secret_ref;
        auto material = resolve_secret_material(reference, cancellation);
        if (!material) {
            return base::Result<
                std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::failure(
                material.error());
        }
        open_request.password = std::move(material).value();
        auto reader = adapters::personal_archive::PersonalFileArchiveReader::open(open_request);
        if (!reader) {
            return base::Result<
                std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::failure(
                map_open_error(reader.error(), true));
        }
        return reader;
    }
    open_request.password = {};
    auto reader = adapters::personal_archive::PersonalFileArchiveReader::open(open_request);
    if (reader) {
        return reader;
    }
    if (reader.error().code == base::ErrorCode::kUnauthorized && connection.credential_ref &&
        connection_allows_default_credential(connection)) {
        auto material = resolve_secret_material(*connection.credential_ref, cancellation);
        if (!material) {
            return base::Result<
                std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::failure(
                material.error());
        }
        open_request.password = std::move(material).value();
        auto retry = adapters::personal_archive::PersonalFileArchiveReader::open(open_request);
        if (!retry) {
            return base::Result<
                std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::failure(
                map_open_error(retry.error(), true));
        }
        return retry;
    }
    return base::Result<std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::
        failure(map_open_error(reader.error(), false));
}

struct BoundContinuation final {
    std::string index_generation;
    std::string parent_entry_id;
    std::string reader_token;
};

[[nodiscard]] base::Result<std::optional<std::string>>
encode_continuation(const std::string& index_generation, const std::string& parent_entry_id,
                    const std::optional<std::string>& reader_token) {
    if (!reader_token) {
        return base::Result<std::optional<std::string>>::success(std::nullopt);
    }
    if (index_generation.empty() || parent_entry_id.empty() || reader_token->empty() ||
        index_generation.find('|') != std::string::npos ||
        parent_entry_id.find('|') != std::string::npos ||
        reader_token->find('|') != std::string::npos) {
        return base::Result<std::optional<std::string>>::failure(
            {base::ErrorCode::kInternal, "continuation binding is invalid"});
    }
    std::string token;
    token.reserve(index_generation.size() + parent_entry_id.size() + reader_token->size() + 2U);
    token.append(index_generation);
    token.push_back('|');
    token.append(parent_entry_id);
    token.push_back('|');
    token.append(*reader_token);
    return base::Result<std::optional<std::string>>::success(std::move(token));
}

[[nodiscard]] base::Result<BoundContinuation>
decode_continuation(const std::optional<std::string>& token, const std::string& expected_parent) {
    if (!token) {
        return base::Result<BoundContinuation>::success(BoundContinuation{});
    }
    const auto first = token->find('|');
    const auto second = first == std::string::npos ? std::string::npos : token->find('|', first + 1U);
    if (first == std::string::npos || second == std::string::npos || first == 0 ||
        second <= first + 1U || second + 1U >= token->size()) {
        return base::Result<BoundContinuation>::failure(
            {base::ErrorCode::kInvalidArgument, std::string(kMessageTokenInvalid)});
    }
    BoundContinuation bound;
    bound.index_generation = token->substr(0, first);
    bound.parent_entry_id = token->substr(first + 1U, second - first - 1U);
    bound.reader_token = token->substr(second + 1U);
    if (bound.parent_entry_id != expected_parent || bound.index_generation.empty() ||
        bound.reader_token.empty()) {
        return base::Result<BoundContinuation>::failure(
            {base::ErrorCode::kInvalidArgument, std::string(kMessageTokenInvalid)});
    }
    return base::Result<BoundContinuation>::success(std::move(bound));
}

[[nodiscard]] contracts::RecoveryPointEntrySummary
project_summary(const contracts::RecoveryPointEntrySummary& item,
                const contracts::FileEntryDesc* described) {
    contracts::RecoveryPointEntrySummary summary = item;
    if (described != nullptr) {
        summary.display_name = project_display_name(described->name);
        summary.entry_kind = described->kind;
        summary.logical_size_bytes = described->logical_size;
    }
    if (summary.display_name.empty()) {
        summary.display_name = ".";
    }
    return summary;
}

[[nodiscard]] base::Result<std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>
open_catalog_file_reader(ports::IControlPlaneDatabase& control_plane,
                         ports::IRepositoryStorageFactory& storage_factory,
                         const contracts::ListRecoveryPointEntriesRequest& request,
                         const base::CancellationToken cancellation) {
    if (!request.repository_connection_id || request.repository_connection_id->empty()) {
        return base::Result<
            std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::failure(
            {base::ErrorCode::kInvalidArgument, "repository_connection_id is required"});
    }
    auto connection =
        control_plane.get_repository_connection(*request.repository_connection_id, cancellation);
    if (!connection) {
        return base::Result<
            std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::failure(
            connection.error());
    }
    if (!connection.value() ||
        connection.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<
            std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    auto storage = storage_factory.open(connection.value()->locator, cancellation);
    if (!storage) {
        return base::Result<
            std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::failure(
            storage.error());
    }
    auto entry = find_catalog_entry(*storage.value(), request.recovery_point_id, cancellation);
    if (!entry) {
        return base::Result<
            std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::failure(
            entry.error());
    }
    if (entry.value().content_kind != personal_repository::kCatalogContentKindFileSet) {
        return base::Result<
            std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::failure(
            {base::ErrorCode::kInvalidArgument, std::string(kMessageContentKind)});
    }
    if (entry.value().structural_state != "complete") {
        return base::Result<
            std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::failure(
            {base::ErrorCode::kConflict, std::string(kMessageCatalogOnly)});
    }
    auto archive_path =
        resolve_archive_path(connection.value()->locator, entry.value().archive_main_key);
    if (!archive_path) {
        return base::Result<
            std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader>>::failure(
            archive_path.error());
    }
    return open_file_recovery_reader(archive_path.value(), request, *connection.value(),
                                     cancellation);
}

[[nodiscard]] base::Result<contracts::RecoveryPointEntryPage>
project_entry_page(ports::IFileRecoveryPointReader& reader,
                   const contracts::ListRecoveryPointEntriesRequest& request,
                   const base::CancellationToken cancellation) {
    const auto index_generation = reader.index_root_digest();
    auto bound = decode_continuation(request.page.continuation_token, request.parent_entry_id);
    if (!bound) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(bound.error());
    }
    if (!bound.value().index_generation.empty() &&
        bound.value().index_generation != index_generation) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(
            {base::ErrorCode::kConflict, std::string(kMessageTokenInvalid)});
    }
    auto parent_id = parse_u64_text(request.parent_entry_id);
    if (!parent_id) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(parent_id.error());
    }
    const auto maximum =
        request.page.maximum_results == 0
            ? contracts::kMaximumServicePageResults
            : (std::min)(request.page.maximum_results, contracts::kMaximumServicePageResults);
    std::optional<std::string> reader_token =
        bound.value().reader_token.empty() ? std::nullopt
                                           : std::optional(bound.value().reader_token);
    auto page = reader.list_children(parent_id.value(), maximum, reader_token, cancellation);
    if (!page) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(page.error());
    }
    contracts::RecoveryPointEntryPage response;
    response.repository_connection_id = request.repository_connection_id;
    response.recovery_point_id = request.recovery_point_id;
    response.parent_entry_id = request.parent_entry_id;
    response.index_generation = index_generation;
    response.items.reserve(page.value().items.size());
    for (const auto& item : page.value().items) {
        auto entry_id = parse_u64_text(item.entry_id);
        if (!entry_id) {
            return base::Result<contracts::RecoveryPointEntryPage>::failure(entry_id.error());
        }
        auto described = reader.describe_entry(entry_id.value(), cancellation);
        if (!described) {
            return base::Result<contracts::RecoveryPointEntryPage>::failure(described.error());
        }
        response.items.push_back(project_summary(item, &described.value()));
    }
    auto continuation =
        encode_continuation(index_generation, request.parent_entry_id, page.value().continuation_token);
    if (!continuation) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(continuation.error());
    }
    response.continuation_token = std::move(continuation).value();
    auto valid_page = contracts::validate_recovery_point_entry_page(response);
    if (!valid_page) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(valid_page.error());
    }
    return base::Result<contracts::RecoveryPointEntryPage>::success(std::move(response));
}

} // namespace

base::Result<contracts::RecoveryPointEntryPage>
list_recovery_point_entries(ports::IControlPlaneDatabase& control_plane,
                            ports::IRepositoryStorageFactory& storage_factory,
                            const contracts::ListRecoveryPointEntriesRequest& request,
                            const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_list_recovery_point_entries_request(request); !valid) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(valid.error());
    }
    auto reader =
        open_catalog_file_reader(control_plane, storage_factory, request, cancellation);
    if (!reader) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(reader.error());
    }
    return project_entry_page(*reader.value(), request, cancellation);
}

} // namespace aegra::apps::service
