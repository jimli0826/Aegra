#include "aegra/application/repository_connection_service.h"

#include "application_ids.h"

#include "aegra/personal_repository/catalog.h"
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

constexpr const char* kCapabilityRepositoryList = "repository.list";
constexpr const char* kRepositoryDescriptorKey = "aegra.repository";

[[nodiscard]] base::Error make_error(const base::ErrorCode code, const char* message) {
    return {code, message};
}

[[nodiscard]] base::Result<void> cancelled(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(make_error(base::ErrorCode::kCancelled, "cancelled"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] std::uint64_t utc_ms(const ports::IClock& clock) {
    const auto now = clock.now_utc_ms();
    return now < 0 ? 0 : static_cast<std::uint64_t>(now);
}

[[nodiscard]] contracts::CommandAcknowledgement accepted(std::string command_id,
                                                         std::optional<std::string> resource_id) {
    contracts::CommandAcknowledgement acknowledgement;
    acknowledgement.command_id = std::move(command_id);
    acknowledgement.disposition = contracts::CommandDisposition::kAccepted;
    acknowledgement.resource_id = std::move(resource_id);
    return acknowledgement;
}

[[nodiscard]] contracts::CommandAcknowledgement replayed(std::string command_id,
                                                         std::optional<std::string> resource_id) {
    auto acknowledgement = accepted(std::move(command_id), std::move(resource_id));
    acknowledgement.disposition = contracts::CommandDisposition::kReplayed;
    return acknowledgement;
}

void append_fingerprint_field(std::string& output, const std::string_view value) {
    output += std::to_string(value.size());
    output.push_back(':');
    output.append(value);
    output.push_back('|');
}

[[nodiscard]] std::string
connection_fingerprint(const std::string_view operation,
                       const contracts::RepositoryConnectionInput& input) {
    std::string fingerprint;
    append_fingerprint_field(fingerprint, operation);
    append_fingerprint_field(fingerprint, input.display_name);
    append_fingerprint_field(fingerprint, input.locator);
    append_fingerprint_field(fingerprint, input.credential_ref ? input.credential_ref->value
                                                               : std::string_view{});
    return fingerprint;
}

[[nodiscard]] std::string resource_fingerprint(const std::string_view operation,
                                               const contracts::ResourceRef& reference) {
    std::string fingerprint;
    append_fingerprint_field(fingerprint, operation);
    append_fingerprint_field(fingerprint, reference.resource_id);
    return fingerprint;
}

[[nodiscard]] base::Result<std::optional<contracts::CommandAcknowledgement>>
load_replay(ports::IControlPlaneDatabase& control_plane, const std::string_view idempotency_key,
            const std::string_view fingerprint, const base::CancellationToken cancellation) {
    auto existing = control_plane.get_command(idempotency_key, cancellation);
    if (!existing) {
        return base::Result<std::optional<contracts::CommandAcknowledgement>>::failure(
            existing.error());
    }
    if (!existing.value()) {
        return base::Result<std::optional<contracts::CommandAcknowledgement>>::success(
            std::nullopt);
    }
    if (existing.value()->request_fingerprint != fingerprint) {
        return base::Result<std::optional<contracts::CommandAcknowledgement>>::failure(
            make_error(base::ErrorCode::kConflict, "idempotency key request mismatch"));
    }
    return base::Result<std::optional<contracts::CommandAcknowledgement>>::success(
        replayed(existing.value()->command_id, existing.value()->resource_id));
}

[[nodiscard]] ports::CommandRecord
make_command_record(const std::string_view idempotency_key, std::string fingerprint,
                    const contracts::CommandAcknowledgement& acknowledgement,
                    const std::uint64_t created_utc_ms) {
    return {std::string(idempotency_key), std::move(fingerprint), acknowledgement.command_id,
            acknowledgement.resource_id, created_utc_ms};
}

[[nodiscard]] base::Result<std::string> new_id(const std::string_view prefix,
                                               ports::IRandomSource& random,
                                               const base::CancellationToken cancellation) {
    return detail::make_random_id(prefix, random, cancellation);
}

[[nodiscard]] base::Result<personal_repository::RepositoryDescriptor>
read_repository_descriptor(ports::IRepositoryStorageAccess& storage,
                           const base::CancellationToken cancellation) {
    auto attributes = storage.reader().get_attributes(kRepositoryDescriptorKey, cancellation);
    if (!attributes) {
        return base::Result<personal_repository::RepositoryDescriptor>::failure(attributes.error());
    }
    if (attributes.value().size_bytes == 0) {
        return base::Result<personal_repository::RepositoryDescriptor>::failure(
            make_error(base::ErrorCode::kCorruptData, "repository descriptor is empty"));
    }
    constexpr std::uint64_t kMaximumDescriptorBytes = 1'048'576;
    if (attributes.value().size_bytes > kMaximumDescriptorBytes) {
        return base::Result<personal_repository::RepositoryDescriptor>::failure(
            make_error(base::ErrorCode::kCorruptData, "repository descriptor is too large"));
    }
    std::vector<std::byte> buffer(static_cast<std::size_t>(attributes.value().size_bytes));
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        auto read = storage.reader().read_range(kRepositoryDescriptorKey, offset,
                                                std::span(buffer).subspan(offset), cancellation);
        if (!read) {
            return base::Result<personal_repository::RepositoryDescriptor>::failure(read.error());
        }
        if (read.value() == 0) {
            return base::Result<personal_repository::RepositoryDescriptor>::failure(
                make_error(base::ErrorCode::kIoFailure, "repository descriptor short read"));
        }
        offset += read.value();
    }
    const auto text = std::string_view(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    auto decoded = personal_repository::decode_repository_descriptor_json(text);
    if (!decoded) {
        return base::Result<personal_repository::RepositoryDescriptor>::failure(decoded.error());
    }
    return decoded;
}

[[nodiscard]] base::Result<void>
probe_repository_descriptor(ports::IRepositoryStorageAccess& storage,
                            const base::CancellationToken cancellation) {
    auto descriptor = read_repository_descriptor(storage, cancellation);
    return descriptor ? base::Result<void>::success()
                      : base::Result<void>::failure(descriptor.error());
}

[[nodiscard]] base::Result<void>
publish_repository_descriptor(ports::IRepositoryStorageAccess& storage,
                              const personal_repository::RepositoryDescriptor& descriptor,
                              const std::string_view encoded,
                              const base::CancellationToken cancellation) {
    const auto staging_key =
        std::string("staging/repository-create/") + descriptor.repository_uuid + ".descriptor";
    auto writer = storage.writer().begin_staged_write(staging_key, cancellation);
    if (!writer) {
        return base::Result<void>::failure(writer.error());
    }
    const auto bytes = std::as_bytes(std::span(encoded.data(), encoded.size()));
    auto written = writer.value()->write(bytes, cancellation);
    auto completed = written ? writer.value()->complete(cancellation)
                             : base::Result<void>::failure(written.error());
    if (!completed) {
        return base::Result<void>::failure(completed.error());
    }
    auto published = storage.publisher().publish(
        {staging_key, kRepositoryDescriptorKey, ports::PublishCondition::kCreateOnly, std::nullopt},
        cancellation);
    if (!published && published.error().code != base::ErrorCode::kOutcomeUnknown) {
        return base::Result<void>::failure(published.error());
    }
    auto actual = read_repository_descriptor(storage, cancellation);
    if (!actual || actual.value() != descriptor) {
        return base::Result<void>::failure(
            !actual ? actual.error()
                    : make_error(base::ErrorCode::kConflict, "repository descriptor changed"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
initialize_repository(ports::IRepositoryStorageFactory& storage_factory, ports::IClock& clock,
                      ports::IRandomSource& random, const std::string_view locator,
                      const base::CancellationToken cancellation) {
    auto repository_uuid = detail::make_random_uuid(random, cancellation);
    if (!repository_uuid) {
        return base::Result<void>::failure(repository_uuid.error());
    }
    personal_repository::RepositoryDescriptor descriptor;
    descriptor.repository_uuid = std::move(repository_uuid).value();
    descriptor.created_utc_ms = utc_ms(clock);
    auto encoded = personal_repository::encode_repository_descriptor_json(descriptor);
    if (!encoded) {
        return base::Result<void>::failure(encoded.error());
    }
    auto storage = storage_factory.create_empty(locator, cancellation);
    if (!storage) {
        return base::Result<void>::failure(storage.error());
    }
    return publish_repository_descriptor(*storage.value(), descriptor, encoded.value(),
                                         cancellation);
}

[[nodiscard]] base::Result<std::optional<ports::RepositoryConnectionRecord>>
find_by_locator(ports::IControlPlaneDatabase& control_plane, const std::string_view locator,
                const base::CancellationToken cancellation) {
    auto page = control_plane.list_repository_connections(
        {{contracts::kMaximumServicePageResults, std::nullopt}, std::nullopt}, cancellation);
    if (!page) {
        return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
            page.error());
    }
    // list returns summaries without locator; scan via get for each is expensive. Enumerate pages
    // of summaries then get full records until locator matches.
    std::optional<std::string> token = page.value().continuation_token;
    std::vector<std::string> ids;
    for (const auto& item : page.value().items) {
        ids.push_back(item.connection_id);
    }
    while (token) {
        auto next = control_plane.list_repository_connections(
            {{contracts::kMaximumServicePageResults, token}, std::nullopt}, cancellation);
        if (!next) {
            return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
                next.error());
        }
        for (const auto& item : next.value().items) {
            ids.push_back(item.connection_id);
        }
        token = next.value().continuation_token;
    }
    for (const auto& id : ids) {
        auto record = control_plane.get_repository_connection(id, cancellation);
        if (!record) {
            return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
                record.error());
        }
        if (record.value() && record.value()->locator == locator) {
            return base::Result<std::optional<ports::RepositoryConnectionRecord>>::success(
                std::move(record.value()));
        }
    }
    return base::Result<std::optional<ports::RepositoryConnectionRecord>>::success(std::nullopt);
}

[[nodiscard]] ports::RepositoryConnectionRecord
make_record(const contracts::RepositoryConnectionInput& input, const std::string& connection_id,
            const std::uint64_t now, const bool is_default,
            const contracts::RepositoryConnectionState state,
            std::vector<std::string> capabilities) {
    ports::RepositoryConnectionRecord record;
    record.connection_id = connection_id;
    record.display_name = input.display_name;
    record.locator = input.locator;
    record.credential_ref = input.credential_ref;
    record.state = state;
    record.is_default = is_default;
    record.capabilities = std::move(capabilities);
    record.created_utc_ms = now;
    record.updated_utc_ms = now;
    return record;
}

[[nodiscard]] base::Result<contracts::CommandAcknowledgement>
persist_existing_connection(ports::IControlPlaneDatabase& control_plane, ports::IClock& clock,
                            ports::IRandomSource& random, const std::string_view connection_id,
                            const std::string_view idempotency_key, std::string fingerprint,
                            const base::CancellationToken cancellation) {
    auto command_id = new_id("cmd-", random, cancellation);
    if (!command_id) {
        return base::Result<contracts::CommandAcknowledgement>::failure(command_id.error());
    }
    auto acknowledgement = replayed(std::move(command_id).value(), std::string(connection_id));
    auto unit = control_plane.begin_unit_of_work(cancellation);
    if (!unit) {
        return base::Result<contracts::CommandAcknowledgement>::failure(unit.error());
    }
    auto stored =
        unit.value()->commands().insert(make_command_record(idempotency_key, std::move(fingerprint),
                                                            acknowledgement, utc_ms(clock)),
                                        cancellation);
    if (!stored) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(stored.error());
    }
    auto committed = unit.value()->commit(cancellation);
    return committed ? base::Result<contracts::CommandAcknowledgement>::success(
                           std::move(acknowledgement))
                     : base::Result<contracts::CommandAcknowledgement>::failure(committed.error());
}

[[nodiscard]] base::Result<contracts::CommandAcknowledgement> create_connection(
    ports::IControlPlaneDatabase& control_plane, ports::IClock& clock, ports::IRandomSource& random,
    const contracts::RepositoryConnectionInput& input, const std::string_view idempotency_key,
    std::string fingerprint, const contracts::RepositoryConnectionState state,
    std::vector<std::string> capabilities, const base::CancellationToken cancellation) {
    auto unit = control_plane.begin_unit_of_work(cancellation);
    if (!unit)
        return base::Result<contracts::CommandAcknowledgement>::failure(unit.error());
    const auto now = utc_ms(clock);
    auto connection_id = new_id("conn-", random, cancellation);
    auto command_id = new_id("cmd-", random, cancellation);
    if (!connection_id || !command_id) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(
            !connection_id ? connection_id.error() : command_id.error());
    }
    auto listed = unit.value()->repository_connections().list({{1, std::nullopt}, std::nullopt},
                                                              cancellation);
    if (!listed) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(listed.error());
    }
    auto record = make_record(input, connection_id.value(), now, listed.value().items.empty(),
                              state, std::move(capabilities));
    auto upserted = unit.value()->repository_connections().upsert(record, cancellation);
    if (!upserted) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(upserted.error());
    }
    auto acknowledgement =
        accepted(std::move(command_id).value(), std::move(connection_id).value());
    auto stored = unit.value()->commands().insert(
        make_command_record(idempotency_key, std::move(fingerprint), acknowledgement, now),
        cancellation);
    if (!stored) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(stored.error());
    }
    auto committed = unit.value()->commit(cancellation);
    return committed ? base::Result<contracts::CommandAcknowledgement>::success(
                           std::move(acknowledgement))
                     : base::Result<contracts::CommandAcknowledgement>::failure(committed.error());
}

[[nodiscard]] base::Result<bool>
repository_available(ports::IRepositoryStorageFactory& storage_factory,
                     const std::string_view locator, const base::CancellationToken cancellation) {
    auto storage = storage_factory.open(locator, cancellation);
    if (!storage) {
        if (storage.error().code == base::ErrorCode::kNotFound ||
            storage.error().code == base::ErrorCode::kIoFailure) {
            return base::Result<bool>::success(false);
        }
        return base::Result<bool>::failure(storage.error());
    }
    auto probed = probe_repository_descriptor(*storage.value(), cancellation);
    if (probed)
        return base::Result<bool>::success(true);
    if (probed.error().code == base::ErrorCode::kCancelled ||
        probed.error().code == base::ErrorCode::kUnauthorized ||
        probed.error().code == base::ErrorCode::kInternal) {
        return base::Result<bool>::failure(probed.error());
    }
    return base::Result<bool>::success(false);
}

struct ConnectionTestPersistence final {
    ports::IControlPlaneDatabase& control_plane;
    ports::IClock& clock;
    ports::IRandomSource& random;
};

struct CommandPersistenceRequest final {
    std::string_view idempotency_key;
    std::string fingerprint;
    base::CancellationToken cancellation;
};

[[nodiscard]] base::Result<contracts::CommandAcknowledgement>
persist_connection_test(const ConnectionTestPersistence dependencies,
                        ports::RepositoryConnectionRecord record, const bool available,
                        CommandPersistenceRequest request) {
    record.state = available ? contracts::RepositoryConnectionState::kAvailable
                             : contracts::RepositoryConnectionState::kUnavailable;
    record.capabilities = available ? std::vector<std::string>{kCapabilityRepositoryList}
                                    : std::vector<std::string>{};
    record.updated_utc_ms = utc_ms(dependencies.clock);
    auto unit = dependencies.control_plane.begin_unit_of_work(request.cancellation);
    if (!unit)
        return base::Result<contracts::CommandAcknowledgement>::failure(unit.error());
    auto upserted = unit.value()->repository_connections().upsert(record, request.cancellation);
    if (!upserted) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(upserted.error());
    }
    if (!available) {
        auto committed = unit.value()->commit(request.cancellation);
        if (!committed)
            return base::Result<contracts::CommandAcknowledgement>::failure(committed.error());
        return base::Result<contracts::CommandAcknowledgement>::failure(
            make_error(base::ErrorCode::kIoFailure, "repository connection test failed"));
    }
    auto command_id = new_id("cmd-", dependencies.random, request.cancellation);
    if (!command_id) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(command_id.error());
    }
    auto acknowledgement = accepted(std::move(command_id).value(), record.connection_id);
    auto stored = unit.value()->commands().insert(
        make_command_record(request.idempotency_key, std::move(request.fingerprint),
                            acknowledgement, utc_ms(dependencies.clock)),
        request.cancellation);
    if (!stored) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(stored.error());
    }
    auto committed = unit.value()->commit(request.cancellation);
    return committed ? base::Result<contracts::CommandAcknowledgement>::success(
                           std::move(acknowledgement))
                     : base::Result<contracts::CommandAcknowledgement>::failure(committed.error());
}

} // namespace

RepositoryConnectionService::RepositoryConnectionService(
    ports::IControlPlaneDatabase& control_plane, ports::IRepositoryStorageFactory& storage_factory,
    ports::IClock& clock, ports::IRandomSource& random) noexcept
    : control_plane_(control_plane), storage_factory_(storage_factory), clock_(clock),
      random_(random) {}

base::Result<contracts::RepositoryConnectionPage> RepositoryConnectionService::list_connections(
    const contracts::RepositoryConnectionListRequest& request,
    const base::CancellationToken cancellation) {
    auto valid = contracts::validate_repository_connection_list_request(request);
    if (!valid) {
        return base::Result<contracts::RepositoryConnectionPage>::failure(valid.error());
    }
    return control_plane_.list_repository_connections(request, cancellation);
}

base::Result<contracts::CommandAcknowledgement>
RepositoryConnectionService::add_connection(const contracts::RepositoryConnectionInput& input,
                                            const std::string_view idempotency_key,
                                            const base::CancellationToken cancellation) {
    auto cancel = cancelled(cancellation);
    if (!cancel) {
        return base::Result<contracts::CommandAcknowledgement>::failure(cancel.error());
    }
    auto key = detail::require_idempotency_key(idempotency_key);
    if (!key) {
        return base::Result<contracts::CommandAcknowledgement>::failure(key.error());
    }
    const auto fingerprint = connection_fingerprint("add", input);
    auto replay = load_replay(control_plane_, idempotency_key, fingerprint, cancellation);
    if (!replay) {
        return base::Result<contracts::CommandAcknowledgement>::failure(replay.error());
    }
    if (replay.value()) {
        return base::Result<contracts::CommandAcknowledgement>::success(*replay.value());
    }
    auto valid = contracts::validate_repository_connection_input(input);
    if (!valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    auto existing = find_by_locator(control_plane_, input.locator, cancellation);
    if (!existing) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
    }
    if (existing.value()) {
        if (existing.value()->display_name == input.display_name &&
            existing.value()->credential_ref == input.credential_ref) {
            if (existing.value()->state == contracts::RepositoryConnectionState::kUnavailable) {
                auto initialized = initialize_repository(storage_factory_, clock_, random_,
                                                         input.locator, cancellation);
                if (!initialized) {
                    return base::Result<contracts::CommandAcknowledgement>::failure(
                        initialized.error());
                }
                return persist_connection_test({control_plane_, clock_, random_},
                                               std::move(*existing.value()), true,
                                               {idempotency_key, fingerprint, cancellation});
            }
            return persist_existing_connection(control_plane_, clock_, random_,
                                               existing.value()->connection_id, idempotency_key,
                                               fingerprint, cancellation);
        }
        return base::Result<contracts::CommandAcknowledgement>::failure(
            make_error(base::ErrorCode::kConflict, "repository locator already registered"));
    }

    auto initialized =
        initialize_repository(storage_factory_, clock_, random_, input.locator, cancellation);
    if (!initialized) {
        return base::Result<contracts::CommandAcknowledgement>::failure(initialized.error());
    }
    return create_connection(control_plane_, clock_, random_, input, idempotency_key, fingerprint,
                             contracts::RepositoryConnectionState::kAvailable,
                             {kCapabilityRepositoryList}, cancellation);
}

base::Result<contracts::CommandAcknowledgement>
RepositoryConnectionService::import_connection(const contracts::RepositoryConnectionInput& input,
                                               const std::string_view idempotency_key,
                                               const base::CancellationToken cancellation) {
    auto cancel = cancelled(cancellation);
    if (!cancel) {
        return base::Result<contracts::CommandAcknowledgement>::failure(cancel.error());
    }
    auto key = detail::require_idempotency_key(idempotency_key);
    if (!key) {
        return base::Result<contracts::CommandAcknowledgement>::failure(key.error());
    }
    const auto fingerprint = connection_fingerprint("import", input);
    auto replay = load_replay(control_plane_, idempotency_key, fingerprint, cancellation);
    if (!replay) {
        return base::Result<contracts::CommandAcknowledgement>::failure(replay.error());
    }
    if (replay.value()) {
        return base::Result<contracts::CommandAcknowledgement>::success(*replay.value());
    }
    auto valid = contracts::validate_repository_connection_input(input);
    if (!valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    auto storage = storage_factory_.open(input.locator, cancellation);
    if (!storage) {
        return base::Result<contracts::CommandAcknowledgement>::failure(storage.error());
    }
    auto probed = probe_repository_descriptor(*storage.value(), cancellation);
    if (!probed) {
        return base::Result<contracts::CommandAcknowledgement>::failure(probed.error());
    }

    auto existing = find_by_locator(control_plane_, input.locator, cancellation);
    if (!existing) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
    }
    if (existing.value()) {
        return persist_existing_connection(control_plane_, clock_, random_,
                                           existing.value()->connection_id, idempotency_key,
                                           fingerprint, cancellation);
    }
    return create_connection(control_plane_, clock_, random_, input, idempotency_key, fingerprint,
                             contracts::RepositoryConnectionState::kAvailable,
                             {kCapabilityRepositoryList}, cancellation);
}

base::Result<contracts::CommandAcknowledgement>
RepositoryConnectionService::test_connection(const contracts::ResourceRef& reference,
                                             const std::string_view idempotency_key,
                                             const base::CancellationToken cancellation) {
    auto cancel = cancelled(cancellation);
    if (!cancel) {
        return base::Result<contracts::CommandAcknowledgement>::failure(cancel.error());
    }
    auto key = detail::require_idempotency_key(idempotency_key);
    if (!key) {
        return base::Result<contracts::CommandAcknowledgement>::failure(key.error());
    }
    const auto fingerprint = resource_fingerprint("test", reference);
    auto replay = load_replay(control_plane_, idempotency_key, fingerprint, cancellation);
    if (!replay) {
        return base::Result<contracts::CommandAcknowledgement>::failure(replay.error());
    }
    if (replay.value()) {
        return base::Result<contracts::CommandAcknowledgement>::success(*replay.value());
    }
    auto valid = contracts::validate_resource_ref(reference);
    if (!valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    auto existing = control_plane_.get_repository_connection(reference.resource_id, cancellation);
    if (!existing) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
    }
    if (!existing.value()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            make_error(base::ErrorCode::kNotFound, "repository connection not found"));
    }
    auto available =
        repository_available(storage_factory_, existing.value()->locator, cancellation);
    if (!available) {
        return base::Result<contracts::CommandAcknowledgement>::failure(available.error());
    }
    return persist_connection_test({control_plane_, clock_, random_}, std::move(*existing.value()),
                                   available.value(), {idempotency_key, fingerprint, cancellation});
}

base::Result<contracts::CommandAcknowledgement>
RepositoryConnectionService::set_default_connection(const contracts::ResourceRef& reference,
                                                    const std::string_view idempotency_key,
                                                    const base::CancellationToken cancellation) {
    auto cancel = cancelled(cancellation);
    if (!cancel) {
        return base::Result<contracts::CommandAcknowledgement>::failure(cancel.error());
    }
    auto key = detail::require_idempotency_key(idempotency_key);
    if (!key) {
        return base::Result<contracts::CommandAcknowledgement>::failure(key.error());
    }
    const auto fingerprint = resource_fingerprint("set-default", reference);
    auto replay = load_replay(control_plane_, idempotency_key, fingerprint, cancellation);
    if (!replay) {
        return base::Result<contracts::CommandAcknowledgement>::failure(replay.error());
    }
    if (replay.value()) {
        return base::Result<contracts::CommandAcknowledgement>::success(*replay.value());
    }
    auto valid = contracts::validate_resource_ref(reference);
    if (!valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    auto unit = control_plane_.begin_unit_of_work(cancellation);
    if (!unit) {
        return base::Result<contracts::CommandAcknowledgement>::failure(unit.error());
    }
    auto updated =
        unit.value()->repository_connections().set_default(reference.resource_id, cancellation);
    if (!updated) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(updated.error());
    }
    auto command_id = new_id("cmd-", random_, cancellation);
    if (!command_id) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(command_id.error());
    }
    auto acknowledgement = accepted(std::move(command_id).value(), reference.resource_id);
    auto stored = unit.value()->commands().insert(
        make_command_record(idempotency_key, fingerprint, acknowledgement, utc_ms(clock_)),
        cancellation);
    if (!stored) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(stored.error());
    }
    auto committed = unit.value()->commit(cancellation);
    if (!committed) {
        return base::Result<contracts::CommandAcknowledgement>::failure(committed.error());
    }
    return base::Result<contracts::CommandAcknowledgement>::success(std::move(acknowledgement));
}

base::Result<contracts::CommandAcknowledgement>
RepositoryConnectionService::remove_connection(const contracts::ResourceRef& reference,
                                               const std::string_view idempotency_key,
                                               const base::CancellationToken cancellation) {
    auto cancel = cancelled(cancellation);
    if (!cancel) {
        return base::Result<contracts::CommandAcknowledgement>::failure(cancel.error());
    }
    auto key = detail::require_idempotency_key(idempotency_key);
    if (!key) {
        return base::Result<contracts::CommandAcknowledgement>::failure(key.error());
    }
    const auto fingerprint = resource_fingerprint("remove", reference);
    auto replay = load_replay(control_plane_, idempotency_key, fingerprint, cancellation);
    if (!replay) {
        return base::Result<contracts::CommandAcknowledgement>::failure(replay.error());
    }
    if (replay.value()) {
        return base::Result<contracts::CommandAcknowledgement>::success(*replay.value());
    }
    auto valid = contracts::validate_resource_ref(reference);
    if (!valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    auto unit = control_plane_.begin_unit_of_work(cancellation);
    if (!unit) {
        return base::Result<contracts::CommandAcknowledgement>::failure(unit.error());
    }
    auto removed =
        unit.value()->repository_connections().remove(reference.resource_id, cancellation);
    if (!removed) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(removed.error());
    }
    auto command_id = new_id("cmd-", random_, cancellation);
    if (!command_id) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(command_id.error());
    }
    auto acknowledgement = accepted(std::move(command_id).value(), reference.resource_id);
    auto stored = unit.value()->commands().insert(
        make_command_record(idempotency_key, fingerprint, acknowledgement, utc_ms(clock_)),
        cancellation);
    if (!stored) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(stored.error());
    }
    auto committed = unit.value()->commit(cancellation);
    if (!committed) {
        return base::Result<contracts::CommandAcknowledgement>::failure(committed.error());
    }
    return base::Result<contracts::CommandAcknowledgement>::success(std::move(acknowledgement));
}

} // namespace aegra::application
