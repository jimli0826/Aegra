#include "file_recovery_chain.h"
#include "worker_job_service_detail.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/catalog_scanner.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegra::apps::service::worker_job_detail {
namespace {

constexpr std::int64_t kVerifySecondsPerItem = 3600;
constexpr std::int64_t kMaximumVerifyDeadlineSeconds = 86'400;

struct VerifyPrepareContext final {
    const contracts::StartVerifyCommand* command{nullptr};
    const std::optional<std::string>* archive_secret_ref{nullptr};
    ports::IControlPlaneDatabase* control_plane{nullptr};
    ports::IRepositoryStorageFactory* storage_factory{nullptr};
    ports::IRandomSource* random{nullptr};
};

[[nodiscard]] base::Result<contracts::SecretRef>
volume_verify_credential(const ports::RepositoryConnectionRecord& repository,
                         const std::string& archive_path_utf8) {
    auto path = path_from_utf8(archive_path_utf8);
    if (!path) {
        return base::Result<contracts::SecretRef>::failure(path.error());
    }
    adapters::personal_archive::ArchiveOpenRequest probe;
    probe.source = std::move(path).value();
    auto opened = adapters::personal_archive::PersonalArchiveReader::open(probe);
    if (opened || opened.error().code != base::ErrorCode::kUnauthorized) {
        return base::Result<contracts::SecretRef>::success({});
    }
    const auto& capabilities = repository.capabilities;
    const bool allows_connection_secret =
        std::find(capabilities.begin(), capabilities.end(), "archive.default_credential") !=
        capabilities.end();
    if (!allows_connection_secret || !repository.credential_ref) {
        return base::Result<contracts::SecretRef>::failure(
            {base::ErrorCode::kUnauthorized, "archive.credential_required"});
    }
    return base::Result<contracts::SecretRef>::success(*repository.credential_ref);
}

[[nodiscard]] std::chrono::seconds verify_deadline(const std::size_t item_count) noexcept {
    const auto count = static_cast<std::int64_t>((std::max)(item_count, std::size_t{1}));
    const auto seconds = kVerifySecondsPerItem * count;
    return std::chrono::seconds{(std::min)(seconds, kMaximumVerifyDeadlineSeconds)};
}

[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
collect_verify_entries(const std::vector<personal_repository::CatalogEntry>& catalog,
                       const std::vector<std::string>& requested_ids) {
    std::unordered_map<std::string, personal_repository::CatalogEntry> by_id;
    by_id.reserve(catalog.size());
    for (const auto& entry : catalog) {
        by_id.emplace(entry.file_uuid, entry);
    }
    std::vector<personal_repository::CatalogEntry> selected;
    selected.reserve(requested_ids.size());
    for (const auto& id : requested_ids) {
        const auto found = by_id.find(id);
        if (found == by_id.end()) {
            return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
                {base::ErrorCode::kNotFound, "recovery point was not found"});
        }
        if (found->second.structural_state != "complete") {
            return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
                {base::ErrorCode::kConflict, "file_recover.catalog_only"});
        }
        selected.push_back(found->second);
    }
    const auto& backup_set = selected.front().backup_set_uuid;
    const auto& content_kind = selected.front().content_kind;
    for (const auto& entry : selected) {
        if (entry.backup_set_uuid != backup_set || entry.content_kind != content_kind) {
            return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
                {base::ErrorCode::kInvalidArgument,
                 "verify recovery points must belong to one backup set"});
        }
    }
    std::sort(selected.begin(), selected.end(),
              [](const personal_repository::CatalogEntry& left,
                 const personal_repository::CatalogEntry& right) {
                  if (left.created_utc_ms != right.created_utc_ms) {
                      return left.created_utc_ms < right.created_utc_ms;
                  }
                  return left.file_uuid < right.file_uuid;
              });
    return base::Result<std::vector<personal_repository::CatalogEntry>>::success(
        std::move(selected));
}

[[nodiscard]] base::Result<std::vector<contracts::SecretRef>>
file_set_credentials(const OpenedFileRecoveryChain& chain, const std::string& job_id) {
    if (chain.password.empty()) {
        return base::Result<std::vector<contracts::SecretRef>>::success(
            std::vector<contracts::SecretRef>(chain.archive_paths_utf8.size()));
    }
    auto protected_secret =
        adapters::windows_system::protect_local_machine_secret(chain.password, job_id);
    if (!protected_secret) {
        return base::Result<std::vector<contracts::SecretRef>>::failure(protected_secret.error());
    }
    return base::Result<std::vector<contracts::SecretRef>>::success(std::vector<contracts::SecretRef>(
        chain.archive_paths_utf8.size(), protected_secret.value()));
}

[[nodiscard]] base::Result<std::vector<std::uint32_t>>
file_set_prefix_lengths(const std::vector<personal_repository::CatalogEntry>& selected,
                        const std::vector<personal_repository::CatalogEntry>& latest_chain) {
    std::unordered_map<std::string, std::uint32_t> prefix_by_id;
    prefix_by_id.reserve(latest_chain.size());
    for (std::size_t index = 0; index < latest_chain.size(); ++index) {
        prefix_by_id.emplace(latest_chain[index].file_uuid,
                             static_cast<std::uint32_t>(index + 1));
    }
    std::vector<std::uint32_t> lengths;
    lengths.reserve(selected.size());
    for (const auto& entry : selected) {
        const auto found = prefix_by_id.find(entry.file_uuid);
        if (found == prefix_by_id.end()) {
            return base::Result<std::vector<std::uint32_t>>::failure(
                {base::ErrorCode::kConflict,
                 "verify recovery points must belong to one backup set chain"});
        }
        lengths.push_back(found->second);
    }
    return base::Result<std::vector<std::uint32_t>>::success(std::move(lengths));
}

[[nodiscard]] PreparedWorkerJob make_verify_job(contracts::JobRequest worker,
                                                std::vector<std::string> recovery_point_ids,
                                                const contracts::StartVerifyCommand& command) {
    worker.operation = contracts::JobOperation::kVerify;
    worker.tenant_id = "personal";
    worker.target_ref.clear();
    worker.verify_recovery_point_ids = recovery_point_ids;
    WorkerJobRequest request;
    request.worker_request = std::move(worker);
    request.source_ids = std::move(recovery_point_ids);
    request.repository_connection_id = command.repository_connection_id;
    request.request_fingerprint = verify_command_fingerprint(command);
    request.deadline = verify_deadline(request.source_ids.size());
    PreparedWorkerJob prepared;
    prepared.job_id = request.worker_request.job_id;
    prepared.request = std::move(request);
    return prepared;
}

[[nodiscard]] base::Result<PreparedWorkerJob>
prepare_file_set_batch(const VerifyPrepareContext& ctx,
                       const std::vector<personal_repository::CatalogEntry>& selected,
                       const base::CancellationToken cancellation) {
    const auto& command = *ctx.command;
    auto chain = open_file_recovery_chain(
        *ctx.control_plane, *ctx.storage_factory, command.repository_connection_id,
        selected.back().file_uuid, *ctx.archive_secret_ref, cancellation);
    if (!chain) {
        return base::Result<PreparedWorkerJob>::failure(chain.error());
    }
    auto lengths = file_set_prefix_lengths(selected, chain.value().catalog_layers);
    if (!lengths) {
        return base::Result<PreparedWorkerJob>::failure(lengths.error());
    }
    auto job_id = random_id("job-", *ctx.random, cancellation);
    auto trace_id = random_id("trace-", *ctx.random, cancellation);
    if (!job_id || !trace_id) {
        return base::Result<PreparedWorkerJob>::failure(!job_id ? job_id.error()
                                                               : trace_id.error());
    }
    auto credentials = file_set_credentials(chain.value(), job_id.value());
    if (!credentials) {
        return base::Result<PreparedWorkerJob>::failure(credentials.error());
    }
    std::vector<std::string> ids;
    ids.reserve(selected.size());
    for (const auto& entry : selected) {
        ids.push_back(entry.file_uuid);
    }
    contracts::JobRequest worker;
    worker.job_id = job_id.value();
    worker.content_kind = contracts::ContentKind::kFileSet;
    worker.source_refs = chain.value().archive_paths_utf8;
    worker.credential_refs = std::move(credentials).value();
    worker.trace_id = std::move(trace_id).value();
    if (ids.size() > 1) {
        worker.verify_chain_lengths = std::move(lengths).value();
    }
    return base::Result<PreparedWorkerJob>::success(
        make_verify_job(std::move(worker), std::move(ids), command));
}

[[nodiscard]] base::Result<PreparedWorkerJob>
prepare_volume_batch(const VerifyPrepareContext& ctx,
                     const ports::RepositoryConnectionRecord& repository,
                     const std::vector<personal_repository::CatalogEntry>& selected,
                     const base::CancellationToken cancellation) {
    auto job_id = random_id("job-", *ctx.random, cancellation);
    auto trace_id = random_id("trace-", *ctx.random, cancellation);
    if (!job_id || !trace_id) {
        return base::Result<PreparedWorkerJob>::failure(!job_id ? job_id.error()
                                                               : trace_id.error());
    }
    contracts::JobRequest worker;
    worker.job_id = job_id.value();
    worker.content_kind = contracts::ContentKind::kVolumeSet;
    worker.trace_id = std::move(trace_id).value();
    std::vector<std::string> ids;
    ids.reserve(selected.size());
    for (const auto& entry : selected) {
        auto archive_path =
            resolve_archive_absolute_path(repository.locator, entry.archive_main_key);
        if (!archive_path) {
            return base::Result<PreparedWorkerJob>::failure(archive_path.error());
        }
        auto credential =
            ctx.archive_secret_ref->has_value()
                ? base::Result<contracts::SecretRef>::success(
                      contracts::SecretRef{ctx.archive_secret_ref->value()})
                : volume_verify_credential(repository, archive_path.value());
        if (!credential) {
            return base::Result<PreparedWorkerJob>::failure(credential.error());
        }
        worker.source_refs.push_back(std::move(archive_path).value());
        worker.credential_refs.push_back(std::move(credential).value());
        ids.push_back(entry.file_uuid);
    }
    return base::Result<PreparedWorkerJob>::success(
        make_verify_job(std::move(worker), std::move(ids), *ctx.command));
}

} // namespace

std::string verify_command_fingerprint(const contracts::StartVerifyCommand& command) {
    auto ids = command.recovery_point_ids;
    std::sort(ids.begin(), ids.end());
    std::string fingerprint = "start-verify|" + command.repository_connection_id;
    for (const auto& id : ids) {
        fingerprint.push_back('|');
        fingerprint += id;
    }
    return fingerprint;
}

base::Result<PreparedWorkerJob>
prepare_verify_job(const contracts::StartVerifyCommand& command,
                   const std::optional<std::string>& archive_secret_ref,
                   const VerifyJobDependencies& dependencies,
                   const base::CancellationToken cancellation) {
    auto repository = dependencies.control_plane->get_repository_connection(
        command.repository_connection_id, cancellation);
    if (!repository) {
        return base::Result<PreparedWorkerJob>::failure(repository.error());
    }
    if (!repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<PreparedWorkerJob>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    auto storage = dependencies.storage_factory->open(repository.value()->locator, cancellation);
    if (!storage) {
        return base::Result<PreparedWorkerJob>::failure(storage.error());
    }
    personal_repository::RepositoryCatalogScanner scanner(storage.value()->reader(),
                                                          storage.value()->enumerator());
    auto loaded = scanner.load_entries(cancellation);
    if (!loaded) {
        return base::Result<PreparedWorkerJob>::failure(loaded.error());
    }
    auto selected = collect_verify_entries(loaded.value().entries, command.recovery_point_ids);
    if (!selected) {
        return base::Result<PreparedWorkerJob>::failure(selected.error());
    }
    const VerifyPrepareContext ctx{&command, &archive_secret_ref, dependencies.control_plane,
                                   dependencies.storage_factory, dependencies.random};
    if (selected.value().front().content_kind == personal_repository::kCatalogContentKindFileSet) {
        return prepare_file_set_batch(ctx, selected.value(), cancellation);
    }
    return prepare_volume_batch(ctx, *repository.value(), selected.value(), cancellation);
}

} // namespace aegra::apps::service::worker_job_detail
