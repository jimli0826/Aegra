#include "file_recovery_chain.h"

#include "worker_job_service_detail.h"

#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/contracts/file_set.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/personal_repository/chain_graph.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

namespace aegra::apps::service {
namespace {

constexpr std::string_view kMessageCredentialRequired = "file_recover.credential_required";
constexpr std::string_view kMessageCredentialFailed = "file_recover.credential_failed";
constexpr std::string_view kMessageCorrupt = "file_recover.corrupt";
constexpr std::string_view kMessageCatalogOnly = "file_recover.catalog_only";
constexpr std::string_view kMessageContentKind = "service.content_kind_mismatch";
constexpr std::string_view kMessageParentMissing = "file_recover.parent_missing";
constexpr std::string_view kMessageParentRefInvalid = "file_recover.parent_reference_invalid";
constexpr std::string_view kMessageChainDepth = "file_recover.chain_depth_limit";

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

[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
load_complete_file_set_chain(ports::IRepositoryStorageAccess& storage,
                             const std::string_view recovery_point_id,
                             const base::CancellationToken cancellation) {
    personal_repository::RepositoryCatalogScanner scanner(storage.reader(), storage.enumerator());
    auto loaded = scanner.load_entries(cancellation);
    if (!loaded) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(loaded.error());
    }
    auto graph = personal_repository::RecoveryPointGraph::build(std::move(loaded).value().entries);
    if (!graph) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(graph.error());
    }
    auto chain = graph.value().resolve_chain(recovery_point_id);
    if (!chain) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(chain.error());
    }
    if (chain.value().empty() || chain.value().back().file_uuid != recovery_point_id) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            {base::ErrorCode::kConflict, std::string(kMessageParentMissing)});
    }
    if (chain.value().size() > contracts::kMaximumFileChainDepth) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            {base::ErrorCode::kInvalidArgument, std::string(kMessageChainDepth)});
    }
    for (const auto& entry : chain.value()) {
        if (entry.content_kind != personal_repository::kCatalogContentKindFileSet) {
            return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
                {base::ErrorCode::kInvalidArgument, std::string(kMessageContentKind)});
        }
        if (entry.structural_state != "complete") {
            return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
                {base::ErrorCode::kConflict, std::string(kMessageCatalogOnly)});
        }
    }
    return base::Result<std::vector<personal_repository::CatalogEntry>>::success(
        std::move(chain).value());
}

[[nodiscard]] base::Result<std::vector<std::string>>
resolve_layer_paths(const std::string& locator,
                    const std::vector<personal_repository::CatalogEntry>& layers) {
    std::vector<std::string> paths;
    paths.reserve(layers.size());
    for (const auto& entry : layers) {
        auto path =
            worker_job_detail::resolve_archive_absolute_path(locator, entry.archive_main_key);
        if (!path) {
            return base::Result<std::vector<std::string>>::failure(path.error());
        }
        paths.push_back(std::move(path).value());
    }
    return base::Result<std::vector<std::string>>::success(std::move(paths));
}

[[nodiscard]] std::filesystem::path path_from_utf8_string(const std::string& value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char item : value) {
        encoded.push_back(static_cast<char8_t>(item));
    }
    return std::filesystem::path(encoded);
}

[[nodiscard]] base::Result<std::unique_ptr<adapters::personal_archive::PersonalFileArchiveChainReader>>
open_chain_layers(const std::vector<std::string>& archive_paths_utf8, const std::string& password) {
    adapters::personal_archive::ArchiveChainOpenRequest open_request;
    open_request.maximum_chain_depth = contracts::kMaximumFileChainDepth;
    open_request.layers.reserve(archive_paths_utf8.size());
    for (const auto& path : archive_paths_utf8) {
        adapters::personal_archive::ArchiveOpenRequest layer;
        layer.source = path_from_utf8_string(path);
        layer.password = password;
        open_request.layers.push_back(std::move(layer));
    }
    return adapters::personal_archive::PersonalFileArchiveChainReader::open(open_request);
}

} // namespace

base::Error map_file_recover_open_error(const base::Error& error, const bool credential_supplied) {
    if (error.code == base::ErrorCode::kUnauthorized) {
        return {base::ErrorCode::kUnauthorized,
                std::string(credential_supplied ? kMessageCredentialFailed
                                                : kMessageCredentialRequired)};
    }
    if (error.message == kMessageParentMissing ||
        error.message.find("parent_uuid") != std::string::npos ||
        error.message.find("parent missing") != std::string::npos ||
        error.message.find("must begin with a full") != std::string::npos) {
        return {base::ErrorCode::kConflict, std::string(kMessageParentMissing)};
    }
    if (error.message.find("parent stream") != std::string::npos ||
        error.message.find("parent_stream") != std::string::npos ||
        error.message.find("parent layer") != std::string::npos) {
        return {base::ErrorCode::kCorruptData, std::string(kMessageParentRefInvalid)};
    }
    if (error.message.find("chain depth") != std::string::npos ||
        error.message.find("chain request is invalid") != std::string::npos) {
        return {base::ErrorCode::kInvalidArgument, std::string(kMessageChainDepth)};
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

base::Result<std::vector<personal_repository::CatalogEntry>>
resolve_file_set_catalog_chain(ports::IControlPlaneDatabase& control_plane,
                               ports::IRepositoryStorageFactory& storage_factory,
                               const std::string_view connection_id,
                               const std::string_view recovery_point_id,
                               const base::CancellationToken cancellation) {
    auto connection = control_plane.get_repository_connection(connection_id, cancellation);
    if (!connection) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            connection.error());
    }
    if (!connection.value() ||
        connection.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    auto storage = storage_factory.open(connection.value()->locator, cancellation);
    if (!storage) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            storage.error());
    }
    return load_complete_file_set_chain(*storage.value(), recovery_point_id, cancellation);
}

base::Result<OpenedFileRecoveryChain>
open_file_recovery_chain(ports::IControlPlaneDatabase& control_plane,
                         ports::IRepositoryStorageFactory& storage_factory,
                         const std::string_view connection_id,
                         const std::string_view recovery_point_id,
                         const std::optional<std::string>& archive_secret_ref,
                         const base::CancellationToken cancellation) {
    auto connection = control_plane.get_repository_connection(connection_id, cancellation);
    if (!connection) {
        return base::Result<OpenedFileRecoveryChain>::failure(connection.error());
    }
    if (!connection.value() ||
        connection.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<OpenedFileRecoveryChain>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    auto storage = storage_factory.open(connection.value()->locator, cancellation);
    if (!storage) {
        return base::Result<OpenedFileRecoveryChain>::failure(storage.error());
    }
    auto layers = load_complete_file_set_chain(*storage.value(), recovery_point_id, cancellation);
    if (!layers) {
        return base::Result<OpenedFileRecoveryChain>::failure(layers.error());
    }
    auto paths = resolve_layer_paths(connection.value()->locator, layers.value());
    if (!paths) {
        return base::Result<OpenedFileRecoveryChain>::failure(paths.error());
    }
    // Credential order matches F7 browse: explicit secret, else empty (unencrypted), else
    // connection default when capability allows.
    std::string password;
    bool credential_supplied = false;
    if (archive_secret_ref && !archive_secret_ref->empty()) {
        contracts::SecretRef reference;
        reference.value = *archive_secret_ref;
        auto material = resolve_secret_material(reference, cancellation);
        if (!material) {
            return base::Result<OpenedFileRecoveryChain>::failure(material.error());
        }
        password = std::move(material).value();
        credential_supplied = true;
    }
    auto reader = open_chain_layers(paths.value(), password);
    if (!reader && reader.error().code == base::ErrorCode::kUnauthorized &&
        !(archive_secret_ref && !archive_secret_ref->empty()) &&
        connection_allows_default_credential(*connection.value()) &&
        connection.value()->credential_ref) {
        auto material =
            resolve_secret_material(*connection.value()->credential_ref, cancellation);
        if (!material) {
            return base::Result<OpenedFileRecoveryChain>::failure(material.error());
        }
        password = std::move(material).value();
        credential_supplied = true;
        reader = open_chain_layers(paths.value(), password);
    }
    if (!reader) {
        return base::Result<OpenedFileRecoveryChain>::failure(
            map_file_recover_open_error(reader.error(), credential_supplied));
    }
    OpenedFileRecoveryChain opened;
    opened.tip_index_digest = reader.value()->index_root_digest();
    opened.chain_generation_digest = reader.value()->chain_generation_digest();
    opened.reader = std::move(reader).value();
    opened.catalog_layers = std::move(layers).value();
    opened.archive_paths_utf8 = std::move(paths).value();
    opened.password = std::move(password);
    opened.repository_locator = connection.value()->locator;
    return base::Result<OpenedFileRecoveryChain>::success(std::move(opened));
}

} // namespace aegra::apps::service
