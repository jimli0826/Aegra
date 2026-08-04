#include "aegra/application/recovery_point_operations.h"

#include "application_ids.h"

#include "aegra/contracts/job.h"
#include "aegra/format/manifest.h"
#include "aegra/personal_repository/chain_graph.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/random.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace aegra::application {
namespace {

constexpr std::uint64_t kDeletePlanTtlMs = 30ULL * 60ULL * 1000ULL;

[[nodiscard]] base::Result<ports::RepositoryConnectionRecord>
require_connection(ports::IControlPlaneDatabase& control_plane, const std::string& connection_id,
                   const base::CancellationToken cancellation) {
    auto found = control_plane.get_repository_connection(connection_id, cancellation);
    if (!found) {
        return base::Result<ports::RepositoryConnectionRecord>::failure(found.error());
    }
    if (!found.value() ||
        found.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<ports::RepositoryConnectionRecord>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    return base::Result<ports::RepositoryConnectionRecord>::success(std::move(*found.value()));
}

[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
load_all_entries(ports::IRepositoryStorageAccess& storage,
                 const personal_repository::CatalogScannerLimits& limits,
                 const base::CancellationToken cancellation) {
    personal_repository::RepositoryCatalogScanner scanner(storage.reader(), storage.enumerator(),
                                                          limits);
    std::vector<personal_repository::CatalogEntry> entries;
    std::optional<std::string> token;
    for (;;) {
        personal_repository::CatalogScanRequest request;
        request.continuation_token = token;
        request.maximum_results = limits.maximum_page_results;
        auto page = scanner.scan(request, cancellation);
        if (!page) {
            return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
                page.error());
        }
        for (auto& point : page.value().recovery_points) {
            entries.push_back(std::move(point.entry));
        }
        if (!page.value().continuation_token) {
            break;
        }
        token = std::move(page.value().continuation_token);
    }
    return base::Result<std::vector<personal_repository::CatalogEntry>>::success(
        std::move(entries));
}

[[nodiscard]] contracts::RecoveryPointChainLayer
map_layer(const personal_repository::CatalogEntry& entry,
          const personal_repository::ChainState chain_state) {
    contracts::RecoveryPointChainLayer layer;
    layer.recovery_point_id = entry.file_uuid;
    layer.backup_type = entry.backup_type == format::BackupType::kIncremental
                            ? contracts::BackupType::kIncremental
                        : entry.backup_type == format::BackupType::kDifferential
                            ? contracts::BackupType::kDifferential
                            : contracts::BackupType::kFull;
    layer.parent_recovery_point_id = entry.parent_uuid;
    layer.structural_state = contracts::RecoveryPointStructuralState::kComplete;
    layer.authentication_state = contracts::RecoveryPointAuthenticationState::kNotAttempted;
    layer.chain_state = chain_state == personal_repository::ChainState::kComplete
                            ? contracts::RecoveryPointChainCompleteness::kComplete
                            : contracts::RecoveryPointChainCompleteness::kIncomplete;
    return layer;
}

[[nodiscard]] std::string plan_object_key(const std::string_view plan_token) {
    return "staging/delete-plans/" + std::string(plan_token) + ".json";
}

[[nodiscard]] base::Result<void> persist_plan(ports::IRepositoryStorageAccess& storage,
                                              const personal_repository::DeletePlan& plan,
                                              const std::string& plan_token,
                                              const base::CancellationToken cancellation) {
    auto encoded = personal_repository::encode_delete_plan_document(plan);
    if (!encoded) {
        return base::Result<void>::failure(encoded.error());
    }
    const auto staging = "staging/" + plan_token + "/plan.json";
    const auto destination = plan_object_key(plan_token);
    auto session = storage.writer().begin_staged_write(staging, cancellation);
    if (!session) {
        return base::Result<void>::failure(session.error());
    }
    const auto bytes = std::as_bytes(std::span(encoded.value().data(), encoded.value().size()));
    auto written = session.value()->write(bytes, cancellation);
    if (!written) {
        session.value()->abort();
        return written;
    }
    auto completed = session.value()->complete(cancellation);
    if (!completed) {
        session.value()->abort();
        return completed;
    }
    ports::ObjectPublishRequest publish;
    publish.staging_key = staging;
    publish.destination_key = destination;
    publish.condition = ports::PublishCondition::kCreateOnly;
    auto published = storage.publisher().publish(publish, cancellation);
    if (!published) {
        return base::Result<void>::failure(published.error());
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<personal_repository::DeletePlan>
load_plan(ports::IRepositoryStorageAccess& storage, const std::string& plan_token,
          const base::CancellationToken cancellation) {
    const auto key = plan_object_key(plan_token);
    auto attributes = storage.reader().get_attributes(key, cancellation);
    if (!attributes) {
        return base::Result<personal_repository::DeletePlan>::failure(attributes.error());
    }
    if (attributes.value().size_bytes == 0 ||
        attributes.value().size_bytes > 4ULL * 1024ULL * 1024ULL) {
        return base::Result<personal_repository::DeletePlan>::failure(
            {base::ErrorCode::kCorruptData, "delete plan document size is invalid"});
    }
    std::string body(static_cast<std::size_t>(attributes.value().size_bytes), '\0');
    std::size_t offset = 0;
    while (offset < body.size()) {
        auto read = storage.reader().read_range(
            key, offset,
            std::as_writable_bytes(std::span(body.data() + offset, body.size() - offset)),
            cancellation);
        if (!read) {
            return base::Result<personal_repository::DeletePlan>::failure(read.error());
        }
        if (read.value() == 0) {
            return base::Result<personal_repository::DeletePlan>::failure(
                {base::ErrorCode::kCorruptData, "delete plan document is truncated"});
        }
        offset += read.value();
    }
    return personal_repository::decode_delete_plan_document(body);
}

[[nodiscard]] base::Result<void> prepare_first_execute(
    ports::IRepositoryStorageAccess& storage, const personal_repository::DeletePlan& plan,
    const personal_repository::CatalogScannerLimits& limits, const bool command_intent_exists,
    const base::CancellationToken cancellation) {
    auto published = personal_repository::load_published_tombstone(
        storage.reader(), plan.tombstone.operation_uuid, cancellation);
    if (!published) {
        return base::Result<void>::failure(published.error());
    }
    if (published.value()) {
        // Resume after partial execute: tombstone is the authority; do not use scanner view.
        if (*published.value() != plan.tombstone) {
            return base::Result<void>::failure(
                {base::ErrorCode::kConflict, "delete tombstone content mismatch on resume"});
        }
        return base::Result<void>::success();
    }
    if (command_intent_exists) {
        bool all_absent = true;
        for (const auto& target : plan.tombstone.targets) {
            for (const auto& member : target.members) {
                auto attributes = storage.reader().get_attributes(member.key, cancellation);
                if (attributes) {
                    all_absent = false;
                    break;
                }
                if (attributes.error().code != base::ErrorCode::kNotFound) {
                    return base::Result<void>::failure(attributes.error());
                }
            }
            if (!all_absent) {
                break;
            }
            auto catalog = storage.reader().get_attributes(
                personal_repository::catalog_entry_key(target.file_uuid), cancellation);
            if (catalog) {
                all_absent = false;
                break;
            }
            if (catalog.error().code != base::ErrorCode::kNotFound) {
                return base::Result<void>::failure(catalog.error());
            }
        }
        if (all_absent) {
            return base::Result<void>::success();
        }
    }
    auto entries = load_all_entries(storage, limits, cancellation);
    if (!entries) {
        return base::Result<void>::failure(entries.error());
    }
    auto strict = personal_repository::revalidate_delete_plan_strict(plan, entries.value());
    if (!strict) {
        return strict;
    }
    return personal_repository::revalidate_delete_plan_members(plan, storage.reader(),
                                                               cancellation);
}

[[nodiscard]] base::Result<contracts::CommandAcknowledgement>
persist_command_intent(ports::IControlPlaneDatabase& control_plane, ports::IRandomSource& random,
                       ports::IClock& clock, const std::string_view idempotency_key,
                       const std::string& fingerprint, const std::string& operation_uuid,
                       const base::CancellationToken cancellation) {
    auto command_id = detail::make_random_id("cmd-", random, cancellation);
    if (!command_id) {
        return base::Result<contracts::CommandAcknowledgement>::failure(command_id.error());
    }
    const auto now = static_cast<std::uint64_t>((std::max)(clock.now_utc_ms(), 0LL));
    auto unit = control_plane.begin_unit_of_work(cancellation);
    if (!unit) {
        return base::Result<contracts::CommandAcknowledgement>::failure(unit.error());
    }
    ports::CommandRecord record{std::string(idempotency_key), fingerprint, command_id.value(),
                                operation_uuid, now};
    auto stored = unit.value()->commands().insert(record, cancellation);
    if (!stored) {
        unit.value()->rollback();
        // Concurrent insert: treat as prior command path.
        if (stored.error().code == base::ErrorCode::kConflict) {
            auto prior = control_plane.get_command(idempotency_key, cancellation);
            if (prior && prior.value() && prior.value()->request_fingerprint == fingerprint) {
                return base::Result<contracts::CommandAcknowledgement>::success(
                    {prior.value()->command_id, contracts::CommandDisposition::kReplayed,
                     prior.value()->resource_id});
            }
        }
        return base::Result<contracts::CommandAcknowledgement>::failure(stored.error());
    }
    auto committed = unit.value()->commit(cancellation);
    if (!committed) {
        return base::Result<contracts::CommandAcknowledgement>::failure(committed.error());
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        {command_id.value(), contracts::CommandDisposition::kAccepted, operation_uuid});
}

[[nodiscard]] base::Result<
    std::pair<std::unique_ptr<ports::IRepositoryStorageAccess>, personal_repository::DeletePlan>>
find_plan(ports::IControlPlaneDatabase& control_plane,
          ports::IRepositoryStorageFactory& storage_factory, const std::string& plan_token,
          const base::CancellationToken cancellation) {
    std::optional<std::string> token;
    for (;;) {
        auto connections = control_plane.list_repository_connections(
            {{contracts::kMaximumServicePageResults, token}, std::nullopt}, cancellation);
        if (!connections) {
            return base::Result<std::pair<std::unique_ptr<ports::IRepositoryStorageAccess>,
                                          personal_repository::DeletePlan>>::failure(connections
                                                                                         .error());
        }
        for (const auto& summary : connections.value().items) {
            auto record =
                control_plane.get_repository_connection(summary.connection_id, cancellation);
            if (!record || !record.value() ||
                record.value()->state != contracts::RepositoryConnectionState::kAvailable) {
                continue;
            }
            auto opened = storage_factory.open(record.value()->locator, cancellation);
            if (!opened) {
                continue;
            }
            auto loaded = load_plan(*opened.value(), plan_token, cancellation);
            if (!loaded || loaded.value().repository_connection_id != summary.connection_id) {
                continue;
            }
            return base::Result<
                std::pair<std::unique_ptr<ports::IRepositoryStorageAccess>,
                          personal_repository::DeletePlan>>::success({std::move(opened).value(),
                                                                      std::move(loaded).value()});
        }
        if (!connections.value().continuation_token) {
            break;
        }
        token = std::move(connections.value().continuation_token);
    }
    return base::Result<
        std::pair<std::unique_ptr<ports::IRepositoryStorageAccess>,
                  personal_repository::DeletePlan>>::failure({base::ErrorCode::kNotFound,
                                                              "delete plan token was not found"});
}

} // namespace

RecoveryPointOperations::RecoveryPointOperations(
    ports::IControlPlaneDatabase& control_plane, ports::IRepositoryStorageFactory& storage_factory,
    ports::IClock& clock, ports::IRandomSource& random,
    personal_repository::CatalogScannerLimits limits) noexcept
    : control_plane_(control_plane), storage_factory_(storage_factory), clock_(clock),
      random_(random), limits_(std::move(limits)) {}

base::Result<contracts::RecoveryPointChainResult>
RecoveryPointOperations::resolve_chain(const contracts::RecoveryPointRef& reference,
                                       const base::CancellationToken cancellation) {
    auto valid = contracts::validate_recovery_point_ref(reference);
    if (!valid) {
        return base::Result<contracts::RecoveryPointChainResult>::failure(valid.error());
    }
    auto connection =
        require_connection(control_plane_, reference.repository_connection_id, cancellation);
    if (!connection) {
        return base::Result<contracts::RecoveryPointChainResult>::failure(connection.error());
    }
    auto storage = storage_factory_.open(connection.value().locator, cancellation);
    if (!storage) {
        return base::Result<contracts::RecoveryPointChainResult>::failure(storage.error());
    }
    auto entries = load_all_entries(*storage.value(), limits_, cancellation);
    if (!entries) {
        return base::Result<contracts::RecoveryPointChainResult>::failure(entries.error());
    }
    auto graph = personal_repository::RecoveryPointGraph::build(entries.value());
    if (!graph) {
        return base::Result<contracts::RecoveryPointChainResult>::failure(graph.error());
    }
    auto chain = graph.value().resolve_chain(reference.recovery_point_id);
    contracts::RecoveryPointChainResult result;
    result.repository_connection_id = reference.repository_connection_id;
    result.recovery_point_id = reference.recovery_point_id;
    if (!chain) {
        if (chain.error().code == base::ErrorCode::kNotFound) {
            return base::Result<contracts::RecoveryPointChainResult>::failure(chain.error());
        }
        result.message_code = "recovery_point.chain_incomplete";
        result.restore_eligible = false;
        result.mount_eligible = false;
        result.verify_eligible = graph.value().find(reference.recovery_point_id) != nullptr;
        if (const auto* leaf = graph.value().find(reference.recovery_point_id)) {
            result.layers.push_back(map_layer(*leaf, personal_repository::ChainState::kIncomplete));
        }
        return base::Result<contracts::RecoveryPointChainResult>::success(std::move(result));
    }
    result.layers.reserve(chain.value().size());
    for (const auto& entry : chain.value()) {
        result.layers.push_back(map_layer(entry, personal_repository::ChainState::kComplete));
    }
    result.restore_eligible = false;
    result.mount_eligible = false;
    result.verify_eligible = true;
    result.message_code = "recovery_point.chain_ready";
    return base::Result<contracts::RecoveryPointChainResult>::success(std::move(result));
}

base::Result<contracts::DeletePlanSummary>
RecoveryPointOperations::plan_delete(const contracts::RecoveryPointRef& reference,
                                     const base::CancellationToken cancellation) {
    auto valid = contracts::validate_recovery_point_ref(reference);
    if (!valid) {
        return base::Result<contracts::DeletePlanSummary>::failure(valid.error());
    }
    auto connection =
        require_connection(control_plane_, reference.repository_connection_id, cancellation);
    if (!connection) {
        return base::Result<contracts::DeletePlanSummary>::failure(connection.error());
    }
    auto storage = storage_factory_.open(connection.value().locator, cancellation);
    if (!storage) {
        return base::Result<contracts::DeletePlanSummary>::failure(storage.error());
    }
    auto entries = load_all_entries(*storage.value(), limits_, cancellation);
    if (!entries) {
        return base::Result<contracts::DeletePlanSummary>::failure(entries.error());
    }
    auto operation = detail::make_random_uuid(random_, cancellation);
    if (!operation) {
        return base::Result<contracts::DeletePlanSummary>::failure(operation.error());
    }
    const auto now = static_cast<std::uint64_t>((std::max)(clock_.now_utc_ms(), 0LL));
    auto plan = personal_repository::plan_delete_recovery_points(
        entries.value(), reference.recovery_point_id, operation.value(), now,
        now + kDeletePlanTtlMs, reference.repository_connection_id,
        [&](const std::string_view key) -> base::Result<std::optional<std::string>> {
            auto attributes = storage.value()->reader().get_attributes(key, cancellation);
            if (!attributes && attributes.error().code == base::ErrorCode::kNotFound) {
                return base::Result<std::optional<std::string>>::success(std::nullopt);
            }
            if (!attributes) {
                return base::Result<std::optional<std::string>>::failure(attributes.error());
            }
            return base::Result<std::optional<std::string>>::success(
                std::move(attributes).value().generation);
        });
    if (!plan) {
        return base::Result<contracts::DeletePlanSummary>::failure(plan.error());
    }
    auto plan_token = detail::make_random_id("plan-", random_, cancellation);
    if (!plan_token) {
        return base::Result<contracts::DeletePlanSummary>::failure(plan_token.error());
    }
    auto persisted = persist_plan(*storage.value(), plan.value(), plan_token.value(), cancellation);
    if (!persisted) {
        return base::Result<contracts::DeletePlanSummary>::failure(persisted.error());
    }
    contracts::DeletePlanSummary summary;
    summary.plan_token = plan_token.value();
    summary.operation_id = plan.value().tombstone.operation_uuid;
    summary.repository_connection_id = reference.repository_connection_id;
    summary.root_recovery_point_id = reference.recovery_point_id;
    summary.expires_utc_ms = plan.value().expires_utc_ms;
    summary.targets.reserve(plan.value().tombstone.targets.size());
    for (const auto& target : plan.value().tombstone.targets) {
        summary.targets.push_back({target.file_uuid, target.catalog_generation,
                                   static_cast<std::uint32_t>(target.members.size())});
    }
    return base::Result<contracts::DeletePlanSummary>::success(std::move(summary));
}

base::Result<contracts::CommandAcknowledgement>
RecoveryPointOperations::execute_delete(const contracts::ExecuteDeletePlanCommand& command,
                                        const std::string_view idempotency_key,
                                        const base::CancellationToken cancellation) {
    auto valid = contracts::validate_execute_delete_plan_command(command);
    if (!valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    const auto fingerprint = "execute-delete|" + command.plan_token;
    auto prior = control_plane_.get_command(idempotency_key, cancellation);
    if (!prior) {
        return base::Result<contracts::CommandAcknowledgement>::failure(prior.error());
    }
    if (prior.value() && prior.value()->request_fingerprint != fingerprint) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "idempotency key conflict"});
    }

    auto found = find_plan(control_plane_, storage_factory_, command.plan_token, cancellation);
    if (!found) {
        return base::Result<contracts::CommandAcknowledgement>::failure(found.error());
    }
    auto storage = std::move(found.value().first);
    auto plan = std::move(found.value().second);

    auto prepared =
        prepare_first_execute(*storage, plan, limits_, prior.value().has_value(), cancellation);
    if (!prepared) {
        return base::Result<contracts::CommandAcknowledgement>::failure(prepared.error());
    }

    const auto now = static_cast<std::uint64_t>((std::max)(clock_.now_utc_ms(), 0LL));
    auto tombstone = personal_repository::load_published_tombstone(
        storage->reader(), plan.tombstone.operation_uuid, cancellation);
    if (!tombstone) {
        return base::Result<contracts::CommandAcknowledgement>::failure(tombstone.error());
    }
    if (!prior.value() && !tombstone.value() && plan.expires_utc_ms <= now) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "delete plan token has expired"});
    }

    contracts::CommandAcknowledgement ack;
    if (prior.value()) {
        ack = {prior.value()->command_id, contracts::CommandDisposition::kReplayed,
               prior.value()->resource_id};
    } else {
        auto intent =
            persist_command_intent(control_plane_, random_, clock_, idempotency_key, fingerprint,
                                   plan.tombstone.operation_uuid, cancellation);
        if (!intent) {
            return intent;
        }
        ack = std::move(intent).value();
    }

    // Tombstone-authoritative execute: safe after partial failure or crash mid-delete.
    personal_repository::DeletePlanExecutor executor(storage->reader(), storage->writer(),
                                                     storage->publisher(), storage->deleter());
    auto executed = executor.execute(plan, cancellation);
    if (!executed) {
        return base::Result<contracts::CommandAcknowledgement>::failure(executed.error());
    }

    // The immutable plan remains the durable replay record after Tombstone cleanup.
    return base::Result<contracts::CommandAcknowledgement>::success(std::move(ack));
}

} // namespace aegra::application
