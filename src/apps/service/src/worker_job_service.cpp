#include "aegra/apps/service/worker_job_service.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/application/source_inventory_query.h"
#include "aegra/apps/service/worker_supervisor.h"
#include "aegra/base/uuid.h"
#include "aegra/format/manifest.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/personal_repository/chain_graph.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/random.h"
#include "aegra/ports/repository_storage.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

[[nodiscard]] base::Result<std::string> random_id(const std::string_view prefix,
                                                  ports::IRandomSource& random,
                                                  const base::CancellationToken cancellation) {
    std::array<std::byte, 16> bytes{};
    if (auto filled = random.fill(bytes, cancellation); !filled) {
        return base::Result<std::string>::failure(filled.error());
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string id(prefix);
    id.reserve(prefix.size() + 32U);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        id.push_back(kHex[value >> 4U]);
        id.push_back(kHex[value & 0x0FU]);
    }
    return base::Result<std::string>::success(std::move(id));
}

[[nodiscard]] base::Result<std::string>
random_uuid(ports::IRandomSource& random, const base::CancellationToken cancellation) {
    std::array<std::byte, 16> bytes{};
    if (auto filled = random.fill(bytes, cancellation); !filled) {
        return base::Result<std::string>::failure(filled.error());
    }
    bytes[6] = static_cast<std::byte>((std::to_integer<unsigned>(bytes[6]) & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::byte>((std::to_integer<unsigned>(bytes[8]) & 0x3FU) | 0x80U);
    return base::Result<std::string>::success(base::format_uuid(bytes));
}

[[nodiscard]] base::Result<std::string> archive_key(const std::string_view file_uuid,
                                                    const std::int64_t created_utc_ms) {
    if (created_utc_ms <= 0) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInternal, "service clock returned invalid time"});
    }
    const auto seconds = static_cast<std::time_t>(created_utc_ms / 1000);
    std::tm utc{};
    if (::gmtime_s(&utc, &seconds) != 0) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInternal, "service clock time is out of range"});
    }
    std::array<char, 96> buffer{};
    const auto count = std::snprintf(buffer.data(), buffer.size(), "archives/%04d/%02d/%.*s.bkf",
                                     utc.tm_year + 1900, utc.tm_mon + 1,
                                     static_cast<int>(file_uuid.size()), file_uuid.data());
    if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size()) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInternal, "archive object key formatting failed"});
    }
    return base::Result<std::string>::success(std::string(buffer.data()));
}

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

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] contracts::CommandAcknowledgement
acknowledgement(std::string command_id, const contracts::CommandDisposition disposition,
                std::optional<std::string> resource_id) {
    return {std::move(command_id), disposition, std::move(resource_id)};
}

/// Wire StartBackup expands to this durable plan (from the schedule) before prepare/submit.
/// backup_type is the *requested* type (wire); effective type may demote Incremental → Full.
struct ResolvedBackupPlan final {
    std::string schedule_id;
    contracts::BackupType backup_type{contracts::BackupType::kFull};
    std::vector<std::string> source_ids;
    std::string repository_connection_id;
    bool exclude_page_and_hibernation_files{true};
    bool encryption_enabled{false};
    std::string backup_set_uuid;
    /// schedules.last_recovery_point_id — sole Incremental parent candidate (no Catalog tip scan).
    std::optional<std::string> last_recovery_point_id;
};

/// Canonical request identity for StartBackup idempotency. Uses requested (not effective) type.
[[nodiscard]] std::string backup_request_fingerprint(const ResolvedBackupPlan& plan) {
    std::string fingerprint = "start-backup|";
    fingerprint += plan.schedule_id;
    fingerprint += "|";
    fingerprint += std::to_string(static_cast<int>(plan.backup_type));
    fingerprint += "|";
    for (const auto& source_id : plan.source_ids) {
        fingerprint += std::to_string(source_id.size());
        fingerprint += ":";
        fingerprint += source_id;
        fingerprint += "|";
    }
    fingerprint += plan.repository_connection_id;
    fingerprint += "|";
    fingerprint += plan.exclude_page_and_hibernation_files ? "1" : "0";
    fingerprint += "|";
    fingerprint += plan.encryption_enabled ? "1" : "0";
    return fingerprint;
}

[[nodiscard]] bool same_backup(const ports::JobRecord& record,
                               const ResolvedBackupPlan& plan) noexcept {
    return record.operation == contracts::JobOperation::kBackup &&
           record.request_fingerprint == backup_request_fingerprint(plan);
}

template <typename Command, typename Matcher>
[[nodiscard]] base::Result<contracts::CommandAcknowledgement>
reconcile_submission_conflict(ports::IControlPlaneDatabase& control_plane,
                              const std::string_view idempotency_key, const Command& command,
                              Matcher&& matches, const base::Error& submission_error,
                              const base::CancellationToken cancellation) {
    if (submission_error.code != base::ErrorCode::kConflict) {
        return base::Result<contracts::CommandAcknowledgement>::failure(submission_error);
    }
    auto existing = control_plane.get_job_by_idempotency_key(idempotency_key, cancellation);
    if (!existing) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
    }
    if (!existing.value()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(submission_error);
    }
    if (!std::forward<Matcher>(matches)(*existing.value(), command)) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "idempotency key request mismatch"});
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        acknowledgement(existing.value()->job_id, contracts::CommandDisposition::kReplayed,
                        existing.value()->job_id));
}

[[nodiscard]] std::string cancel_fingerprint(const std::string_view job_id) {
    return "cancel|" + std::to_string(job_id.size()) + ":" + std::string(job_id);
}

[[nodiscard]] base::Result<std::string>
resolve_archive_absolute_path(const std::string& locator, const std::string& archive_main_key) {
    auto root = path_from_utf8(locator);
    if (!root) {
        return base::Result<std::string>::failure(root.error());
    }
    if (!archive_main_key.starts_with("archives/") ||
        archive_main_key.find('\\') != std::string::npos ||
        archive_main_key.find(':') != std::string::npos ||
        archive_main_key.find("..") != std::string::npos) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "archive key is outside the archive root"});
    }
    std::filesystem::path relative;
    try {
        relative = std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(archive_main_key.data()), archive_main_key.size()));
    } catch (const std::exception&) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "archive key is invalid"});
    }
    std::error_code error_code;
    const auto canonical_root = std::filesystem::weakly_canonical(root.value(), error_code);
    if (error_code) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kIoFailure, "repository root cannot be resolved"});
    }
    const auto canonical_archive =
        std::filesystem::weakly_canonical(canonical_root / relative, error_code);
    if (error_code) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kIoFailure, "archive path cannot be resolved"});
    }
    const auto relative_to_root = canonical_archive.lexically_relative(canonical_root);
    if (relative_to_root.empty() || relative_to_root == L".." ||
        relative_to_root.native().starts_with(
            L".." + std::wstring(1, std::filesystem::path::preferred_separator))) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kConflict, "archive path escapes repository root"});
    }
    return base::Result<std::string>::success(path_to_utf8(canonical_archive));
}

[[nodiscard]] ports::JobStateTransition cancelling_transition(const std::string_view job_id,
                                                              const std::uint64_t utc_ms) {
    ports::JobStateTransition transition;
    transition.job_id = std::string(job_id);
    transition.expected_state = contracts::ServiceJobState::kRunning;
    transition.next_state = contracts::ServiceJobState::kCancelling;
    transition.transition_utc_ms = utc_ms;
    transition.message_code = "job.cancelling";
    return transition;
}

struct PreparedBackup final {
    WorkerJobRequest request;
    std::string job_id;
};

[[nodiscard]] bool is_chainable_parent_entry(
    const personal_repository::CatalogEntry& entry,
    const std::vector<std::string>& source_volume_ids) noexcept {
    return entry.has_sidecar && entry.structural_state == "complete" &&
           entry.source_volume_ids == source_volume_ids &&
           (entry.backup_type == format::BackupType::kFull ||
            entry.backup_type == format::BackupType::kIncremental);
}

[[nodiscard]] base::Result<ResolvedBackupPlan>
resolve_backup_plan(ports::IControlPlaneDatabase& control_plane,
                    const contracts::StartBackupCommand& command,
                    const base::CancellationToken cancellation) {
    auto schedule = control_plane.get_schedule(command.schedule_id, cancellation);
    if (!schedule) {
        return base::Result<ResolvedBackupPlan>::failure(schedule.error());
    }
    if (!schedule.value()) {
        return base::Result<ResolvedBackupPlan>::failure(
            {base::ErrorCode::kNotFound, "schedule was not found"});
    }
    const auto& record = *schedule.value();
    if (!base::is_canonical_uuid(record.backup_set_uuid)) {
        return base::Result<ResolvedBackupPlan>::failure(
            {base::ErrorCode::kCorruptData, "schedule backup set identity is invalid"});
    }
    if (record.encryption_enabled && record.archive_password_protected.empty()) {
        return base::Result<ResolvedBackupPlan>::failure(
            {base::ErrorCode::kUnauthorized, "encrypted schedule is missing a protected password"});
    }
    ResolvedBackupPlan plan;
    plan.schedule_id = command.schedule_id;
    plan.backup_type = command.backup_type;
    plan.source_ids = record.source_ids;
    plan.repository_connection_id = record.repository_connection_id;
    plan.exclude_page_and_hibernation_files = record.exclude_page_and_hibernation_files;
    plan.encryption_enabled = record.encryption_enabled;
    plan.backup_set_uuid = record.backup_set_uuid;
    plan.last_recovery_point_id = record.last_recovery_point_id;
    return base::Result<ResolvedBackupPlan>::success(std::move(plan));
}

/// parent set → run Incremental; parent empty → demote to Full.
struct IncrementalParentResolution final {
    std::optional<personal_repository::CatalogEntry> parent;
    std::optional<std::string> retained_backup_set_uuid;
};

[[nodiscard]] IncrementalParentResolution demote_to_full(std::optional<std::string> set_uuid) {
    return {std::nullopt, std::move(set_uuid)};
}

[[nodiscard]] base::Result<std::optional<personal_repository::CatalogEntry>>
read_catalog_entry_by_uuid(ports::IObjectReader& reader, const std::string& catalog_prefix,
                           const std::string& file_uuid,
                           const base::CancellationToken cancellation) {
    const auto key = catalog_prefix + "/" + file_uuid + ".entry";
    auto attributes = reader.get_attributes(key, cancellation);
    if (!attributes) {
        if (attributes.error().code == base::ErrorCode::kNotFound) {
            return base::Result<std::optional<personal_repository::CatalogEntry>>::success(
                std::nullopt);
        }
        return base::Result<std::optional<personal_repository::CatalogEntry>>::failure(
            attributes.error());
    }
    if (attributes.value().size_bytes > 4ULL * 1024ULL * 1024ULL) {
        return base::Result<std::optional<personal_repository::CatalogEntry>>::failure(
            {base::ErrorCode::kCorruptData, "catalog entry is too large"});
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(attributes.value().size_bytes));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto read =
            reader.read_range(key, offset, std::span(bytes).subspan(offset), cancellation);
        if (!read || read.value() == 0) {
            return base::Result<std::optional<personal_repository::CatalogEntry>>::failure(
                !read ? read.error()
                      : base::Error{base::ErrorCode::kIoFailure, "catalog entry was short read"});
        }
        offset += read.value();
    }
    const auto text =
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    auto decoded = personal_repository::decode_catalog_entry_json(text);
    if (!decoded) {
        return base::Result<std::optional<personal_repository::CatalogEntry>>::failure(
            decoded.error());
    }
    return base::Result<std::optional<personal_repository::CatalogEntry>>::success(
        std::move(decoded).value());
}

/// Walk parent_uuid from last_rp; incomplete/invalid → demote (no Catalog tip rescan).
[[nodiscard]] base::Result<bool>
ancestor_chain_complete(ports::IObjectReader& reader, const std::string& catalog_prefix,
                        personal_repository::CatalogEntry candidate,
                        const std::string& schedule_backup_set_uuid,
                        const base::CancellationToken cancellation) {
    constexpr std::uint32_t kMaximumChainDepth = 128;
    std::vector<std::string> seen;
    seen.reserve(kMaximumChainDepth);
    personal_repository::CatalogEntry current = std::move(candidate);
    for (std::uint32_t depth = 0; depth < kMaximumChainDepth; ++depth) {
        if (current.backup_set_uuid != schedule_backup_set_uuid) {
            return base::Result<bool>::success(false);
        }
        if (std::find(seen.begin(), seen.end(), current.file_uuid) != seen.end()) {
            return base::Result<bool>::success(false);
        }
        seen.push_back(current.file_uuid);
        if (!current.parent_uuid) {
            return base::Result<bool>::success(current.backup_type == format::BackupType::kFull);
        }
        auto parent = read_catalog_entry_by_uuid(reader, catalog_prefix, *current.parent_uuid,
                                                cancellation);
        if (!parent) {
            return base::Result<bool>::failure(parent.error());
        }
        if (!parent.value()) {
            return base::Result<bool>::success(false);
        }
        current = std::move(*parent.value());
    }
    return base::Result<bool>::success(false);
}

/// Parent is only schedules.last_recovery_point_id. Empty/missing/invalid chain → demote Full.
[[nodiscard]] base::Result<IncrementalParentResolution>
resolve_incremental_parent(ports::IRepositoryStorageAccess& storage,
                           const std::string& schedule_backup_set_uuid,
                           const std::optional<std::string>& last_recovery_point_id,
                           const std::vector<std::string>& source_volume_ids,
                           const base::CancellationToken cancellation) {
    if (!last_recovery_point_id || last_recovery_point_id->empty()) {
        return base::Result<IncrementalParentResolution>::success(
            demote_to_full(std::optional<std::string>{schedule_backup_set_uuid}));
    }
    // Prefix is fixed by repository format; tip identity comes from the schedule, not a scan.
    constexpr std::string_view kCatalogPrefix = "catalog/recovery-points";
    auto entry = read_catalog_entry_by_uuid(storage.reader(), std::string(kCatalogPrefix),
                                           *last_recovery_point_id, cancellation);
    if (!entry) {
        return base::Result<IncrementalParentResolution>::failure(entry.error());
    }
    if (!entry.value() || !is_chainable_parent_entry(*entry.value(), source_volume_ids) ||
        entry.value()->backup_set_uuid != schedule_backup_set_uuid) {
        return base::Result<IncrementalParentResolution>::success(
            demote_to_full(std::optional<std::string>{schedule_backup_set_uuid}));
    }
    auto complete = ancestor_chain_complete(storage.reader(), std::string(kCatalogPrefix),
                                            *entry.value(), schedule_backup_set_uuid, cancellation);
    if (!complete) {
        return base::Result<IncrementalParentResolution>::failure(complete.error());
    }
    if (!complete.value()) {
        return base::Result<IncrementalParentResolution>::success(
            demote_to_full(std::optional<std::string>{schedule_backup_set_uuid}));
    }
    return base::Result<IncrementalParentResolution>::success(
        IncrementalParentResolution{*entry.value(), std::nullopt});
}

[[nodiscard]] base::Result<void>
assign_backup_credentials(contracts::JobRequest& worker, const ResolvedBackupPlan& plan,
                          ports::IControlPlaneDatabase& control_plane,
                          const base::CancellationToken cancellation) {
    if (!plan.encryption_enabled) {
        return base::Result<void>::success();
    }
    auto schedule = control_plane.get_schedule(plan.schedule_id, cancellation);
    if (!schedule) {
        return base::Result<void>::failure(schedule.error());
    }
    if (!schedule.value() || schedule.value()->archive_password_protected.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kUnauthorized, "encrypted schedule is missing a protected password"});
    }
    // Ciphertext was protected with schedule_id as pOptionalEntropy at create time.
    contracts::SecretRef reference;
    reference.value = schedule.value()->archive_password_protected;
    worker.credential_refs = {std::move(reference)};
    return base::Result<void>::success();
}

struct PrepareBackupContext final {
    application::ISourceInventoryQuery& source_inventory;
    ports::IControlPlaneDatabase& control_plane;
    ports::IRepositoryStorageFactory& storage_factory;
    ports::IClock& clock;
    ports::IRandomSource& random;
};

struct BackupIdentity final {
    std::string job_id;
    std::string trace_id;
    std::string file_uuid;
    std::string archive_key;
    std::filesystem::path archive_path;
    std::int64_t created_utc_ms{0};
};

[[nodiscard]] base::Result<std::vector<std::string>>
resolve_backup_source_refs(const ResolvedBackupPlan& plan,
                           application::ISourceInventoryQuery& source_inventory,
                           const base::CancellationToken cancellation) {
    std::vector<std::string> stable_source_refs;
    stable_source_refs.reserve(plan.source_ids.size());
    for (const auto& source_id : plan.source_ids) {
        auto source = source_inventory.resolve_source(source_id, cancellation);
        if (!source) {
            return base::Result<std::vector<std::string>>::failure(source.error());
        }
        stable_source_refs.push_back(std::move(source).value().stable_key);
    }
    return base::Result<std::vector<std::string>>::success(std::move(stable_source_refs));
}

[[nodiscard]] base::Result<ports::RepositoryConnectionRecord>
load_available_repository(ports::IControlPlaneDatabase& control_plane,
                          const std::string& connection_id,
                          const base::CancellationToken cancellation) {
    auto repository = control_plane.get_repository_connection(connection_id, cancellation);
    if (!repository) {
        return base::Result<ports::RepositoryConnectionRecord>::failure(repository.error());
    }
    if (!repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<ports::RepositoryConnectionRecord>::failure(
            {base::ErrorCode::kConflict, "repository is unavailable"});
    }
    return base::Result<ports::RepositoryConnectionRecord>::success(
        std::move(*repository.value()));
}

[[nodiscard]] base::Result<IncrementalParentResolution>
maybe_resolve_parent(const ResolvedBackupPlan& plan,
                     const std::vector<std::string>& source_volume_ids,
                     const ports::RepositoryConnectionRecord& repository,
                     ports::IRepositoryStorageFactory& storage_factory,
                     const base::CancellationToken cancellation) {
    if (plan.backup_type != contracts::BackupType::kIncremental) {
        return base::Result<IncrementalParentResolution>::success(
            IncrementalParentResolution{});
    }
    auto storage = storage_factory.open(repository.locator, cancellation);
    if (!storage) {
        return base::Result<IncrementalParentResolution>::failure(storage.error());
    }
    // last_rp only; missing/invalid tip or incomplete chain → demote Full (no Catalog tip scan).
    return resolve_incremental_parent(*storage.value(), plan.backup_set_uuid,
                                      plan.last_recovery_point_id, source_volume_ids, cancellation);
}

[[nodiscard]] base::Result<BackupIdentity>
allocate_backup_identity(const std::string& repository_locator, ports::IClock& clock,
                         ports::IRandomSource& random, const base::CancellationToken cancellation) {
    auto root = path_from_utf8(repository_locator);
    auto job_id = random_id("job-", random, cancellation);
    auto trace_id = random_id("trace-", random, cancellation);
    auto file_uuid = random_uuid(random, cancellation);
    const auto created_utc_ms = clock.now_utc_ms();
    auto key = file_uuid ? archive_key(file_uuid.value(), created_utc_ms)
                         : base::Result<std::string>::failure(file_uuid.error());
    if (!root || !job_id || !trace_id || !file_uuid || !key) {
        if (!root) {
            return base::Result<BackupIdentity>::failure(root.error());
        }
        if (!job_id || !trace_id) {
            return base::Result<BackupIdentity>::failure(!job_id ? job_id.error()
                                                                 : trace_id.error());
        }
        if (!file_uuid) {
            return base::Result<BackupIdentity>::failure(file_uuid.error());
        }
        return base::Result<BackupIdentity>::failure(key.error());
    }
    BackupIdentity identity;
    identity.job_id = std::move(job_id).value();
    identity.trace_id = std::move(trace_id).value();
    identity.file_uuid = std::move(file_uuid).value();
    identity.archive_key = std::move(key).value();
    identity.archive_path = root.value() / std::filesystem::path(identity.archive_key);
    identity.created_utc_ms = created_utc_ms;
    std::error_code error_code;
    std::filesystem::create_directories(identity.archive_path.parent_path(), error_code);
    if (error_code) {
        return base::Result<BackupIdentity>::failure(
            {base::ErrorCode::kIoFailure, "archive directory create failed"});
    }
    return base::Result<BackupIdentity>::success(std::move(identity));
}

struct BackupOptionsInput final {
    const ResolvedBackupPlan& plan;
    const BackupIdentity& identity;
    const IncrementalParentResolution& parent_resolution;
    const std::string& repository_locator;
};

[[nodiscard]] base::Result<contracts::BackupOptions>
make_backup_options(const BackupOptionsInput& input) {
    contracts::BackupOptions backup;
    backup.file_uuid = input.identity.file_uuid;
    backup.created_utc_ms = input.identity.created_utc_ms;
    backup.exclude_page_and_hibernation_files = input.plan.exclude_page_and_hibernation_files;
    backup.encryption_enabled = input.plan.encryption_enabled;
    if (!input.parent_resolution.parent) {
        // Full (requested or demoted from Incremental when the parent chain is unusable).
        backup.type = contracts::BackupType::kFull;
        // Schedule always owns the backup set identity.
        backup.backup_set_uuid = input.plan.backup_set_uuid;
        return base::Result<contracts::BackupOptions>::success(std::move(backup));
    }
    backup.type = contracts::BackupType::kIncremental;
    auto parent_path = resolve_archive_absolute_path(
        input.repository_locator, input.parent_resolution.parent->archive_main_key);
    if (!parent_path) {
        return base::Result<contracts::BackupOptions>::failure(parent_path.error());
    }
    backup.parent_source_ref = std::move(parent_path).value();
    // Incremental inherits backup_set_uuid from the parent archive at write time.
    backup.backup_set_uuid.clear();
    return base::Result<contracts::BackupOptions>::success(std::move(backup));
}

[[nodiscard]] base::Result<PreparedBackup>
prepare_backup(const ResolvedBackupPlan& plan, PrepareBackupContext& context,
               const base::CancellationToken cancellation) {
    auto sources = resolve_backup_source_refs(plan, context.source_inventory, cancellation);
    if (!sources) {
        return base::Result<PreparedBackup>::failure(sources.error());
    }
    auto repository = load_available_repository(context.control_plane,
                                                plan.repository_connection_id, cancellation);
    if (!repository) {
        return base::Result<PreparedBackup>::failure(repository.error());
    }
    auto parent =
        maybe_resolve_parent(plan, sources.value(), repository.value(), context.storage_factory,
                             cancellation);
    if (!parent) {
        return base::Result<PreparedBackup>::failure(parent.error());
    }
    auto identity = allocate_backup_identity(repository.value().locator, context.clock,
                                             context.random, cancellation);
    if (!identity) {
        return base::Result<PreparedBackup>::failure(identity.error());
    }
    const BackupOptionsInput options_input{plan, identity.value(), parent.value(),
                                           repository.value().locator};
    auto backup = make_backup_options(options_input);
    if (!backup) {
        return base::Result<PreparedBackup>::failure(backup.error());
    }
    contracts::JobRequest worker;
    worker.job_id = identity.value().job_id;
    worker.tenant_id = "personal";
    worker.operation = contracts::JobOperation::kBackup;
    worker.source_refs = std::move(sources).value();
    worker.target_ref = path_to_utf8(identity.value().archive_path);
    worker.trace_id = identity.value().trace_id;
    auto credentials =
        assign_backup_credentials(worker, plan, context.control_plane, cancellation);
    if (!credentials) {
        return base::Result<PreparedBackup>::failure(credentials.error());
    }
    worker.backup = std::move(backup).value();
    std::optional<std::string> parent_id;
    if (parent.value().parent) {
        parent_id = parent.value().parent->file_uuid;
    }
    WorkerJobRequest request;
    request.worker_request = std::move(worker);
    request.source_ids = plan.source_ids;
    request.repository_connection_id = plan.repository_connection_id;
    request.parent_recovery_point_id = std::move(parent_id);
    request.request_fingerprint = backup_request_fingerprint(plan);
    request.schedule_id = plan.schedule_id;
    request.backup_archive_key = identity.value().archive_key;
    return base::Result<PreparedBackup>::success(
        {std::move(request), std::move(identity).value().job_id});
}

[[nodiscard]] base::Result<contracts::CommandAcknowledgement>
persist_cancel_command(ports::IControlPlaneDatabase& control_plane, ports::IClock& clock,
                       ports::IRandomSource& random, const ports::JobRecord& current,
                       const contracts::ResourceRef& job, const std::string_view idempotency_key,
                       std::string fingerprint, const base::CancellationToken cancellation) {
    auto command_id = random_id("cmd-", random, cancellation);
    if (!command_id) {
        return base::Result<contracts::CommandAcknowledgement>::failure(command_id.error());
    }
    auto result = acknowledgement(command_id.value(), contracts::CommandDisposition::kAccepted,
                                  job.resource_id);
    auto unit = control_plane.begin_unit_of_work(cancellation);
    if (!unit)
        return base::Result<contracts::CommandAcknowledgement>::failure(unit.error());
    const auto now = static_cast<std::uint64_t>((std::max)(clock.now_utc_ms(), 0LL));
    if (current.state == contracts::ServiceJobState::kRunning) {
        auto transitioned = unit.value()->jobs().transition(
            cancelling_transition(job.resource_id, now), cancellation);
        if (!transitioned) {
            unit.value()->rollback();
            return base::Result<contracts::CommandAcknowledgement>::failure(transitioned.error());
        }
    }
    ports::CommandRecord record{std::string(idempotency_key), std::move(fingerprint),
                                command_id.value(), job.resource_id, now};
    auto stored = unit.value()->commands().insert(record, cancellation);
    if (!stored) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(stored.error());
    }
    auto committed = unit.value()->commit(cancellation);
    return committed ? base::Result<contracts::CommandAcknowledgement>::success(std::move(result))
                     : base::Result<contracts::CommandAcknowledgement>::failure(committed.error());
}

} // namespace

[[nodiscard]] std::string verify_request_fingerprint(const contracts::StartVerifyCommand& command) {
    return "start-verify|" + command.repository_connection_id + "|" + command.recovery_point_id;
}

[[nodiscard]] bool same_verify(const ports::JobRecord& record,
                               const contracts::StartVerifyCommand& command) noexcept {
    return record.operation == contracts::JobOperation::kVerify &&
           record.request_fingerprint == verify_request_fingerprint(command);
}

[[nodiscard]] base::Result<PreparedBackup>
prepare_verify(const contracts::StartVerifyCommand& command,
               ports::IControlPlaneDatabase& control_plane,
               ports::IRepositoryStorageFactory& storage_factory, ports::IRandomSource& random,
               const base::CancellationToken cancellation) {
    auto repository =
        control_plane.get_repository_connection(command.repository_connection_id, cancellation);
    if (!repository) {
        return base::Result<PreparedBackup>::failure(repository.error());
    }
    if (!repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<PreparedBackup>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    auto storage = storage_factory.open(repository.value()->locator, cancellation);
    if (!storage) {
        return base::Result<PreparedBackup>::failure(storage.error());
    }
    personal_repository::RepositoryCatalogScanner scanner(storage.value()->reader(),
                                                          storage.value()->enumerator());
    std::optional<std::string> token;
    std::optional<personal_repository::CatalogEntry> found;
    for (;;) {
        personal_repository::CatalogScanRequest request;
        request.continuation_token = token;
        request.maximum_results = 100;
        auto page = scanner.scan(request, cancellation);
        if (!page) {
            return base::Result<PreparedBackup>::failure(page.error());
        }
        for (const auto& point : page.value().recovery_points) {
            if (point.entry.file_uuid == command.recovery_point_id) {
                found = point.entry;
                break;
            }
        }
        if (found || !page.value().continuation_token) {
            break;
        }
        token = std::move(page.value().continuation_token);
    }
    if (!found) {
        return base::Result<PreparedBackup>::failure(
            {base::ErrorCode::kNotFound, "recovery point was not found"});
    }
    // Archive credential selection: never reuse a connection SecretRef without an explicit
    // repository_uuid+file_uuid mapping. Mapping store is not yet durable; refuse with
    // credential_required rather than guessing with the repository connection credential.
    static_cast<void>(found->repository_uuid);
    if (!repository.value()->credential_ref) {
        return base::Result<PreparedBackup>::failure(
            {base::ErrorCode::kUnauthorized, "archive.credential_required"});
    }
    // Explicit mapping table is not available yet. Connection credential is only valid when the
    // connection advertises archive.default_credential (Service-managed single-secret repos).
    // Import/connections without that capability must register a per-file mapping (future S5+).
    const auto& capabilities = repository.value()->capabilities;
    const bool allows_connection_secret =
        std::find(capabilities.begin(), capabilities.end(), "archive.default_credential") !=
        capabilities.end();
    if (!allows_connection_secret) {
        return base::Result<PreparedBackup>::failure(
            {base::ErrorCode::kUnauthorized, "archive.credential_required"});
    }
    auto archive_path =
        resolve_archive_absolute_path(repository.value()->locator, found->archive_main_key);
    auto job_id = random_id("job-", random, cancellation);
    auto trace_id = random_id("trace-", random, cancellation);
    if (!archive_path || !job_id || !trace_id) {
        if (!archive_path)
            return base::Result<PreparedBackup>::failure(archive_path.error());
        return base::Result<PreparedBackup>::failure(!job_id ? job_id.error() : trace_id.error());
    }
    contracts::JobRequest worker;
    worker.job_id = job_id.value();
    worker.tenant_id = "personal";
    worker.operation = contracts::JobOperation::kVerify;
    worker.source_refs = {archive_path.value()};
    worker.target_ref.clear();
    worker.credential_refs = {*repository.value()->credential_ref};
    worker.trace_id = trace_id.value();
    WorkerJobRequest request;
    request.worker_request = std::move(worker);
    request.source_ids = {command.recovery_point_id};
    request.repository_connection_id = command.repository_connection_id;
    request.request_fingerprint = verify_request_fingerprint(command);
    return base::Result<PreparedBackup>::success({std::move(request), std::move(job_id).value()});
}

constexpr std::uint64_t kRestorePreflightTtlMs = 5U * 60U * 1'000U;
constexpr std::string_view kDiskSourcePrefix = "disk.";

[[nodiscard]] bool is_disk_target_id(const std::string_view source_id) noexcept {
    return source_id.starts_with(kDiskSourcePrefix) && source_id.size() > kDiskSourcePrefix.size();
}

[[nodiscard]] std::optional<std::uint32_t>
parse_disk_source_number(const std::string_view source_id) noexcept {
    if (!is_disk_target_id(source_id)) {
        return std::nullopt;
    }
    const auto digits = source_id.substr(kDiskSourcePrefix.size());
    if (digits.empty() || !std::ranges::all_of(digits, [](const unsigned char ch) {
            return std::isdigit(ch) != 0;
        })) {
        return std::nullopt;
    }
    std::uint32_t number = 0;
    const auto* begin = digits.data();
    const auto* end = begin + digits.size();
    if (std::from_chars(begin, end, number).ec != std::errc{}) {
        return std::nullopt;
    }
    return number;
}

[[nodiscard]] base::Result<contracts::SourceInventoryItem>
find_inventory_item(application::ISourceInventoryQuery& inventory, const std::string_view source_id,
                    const base::CancellationToken cancellation) {
    contracts::SourceInventoryListRequest request;
    request.include_unavailable = true;
    request.page.maximum_results = contracts::kMaximumServicePageResults;
    auto page = inventory.list_sources(request, cancellation);
    if (!page) {
        return base::Result<contracts::SourceInventoryItem>::failure(page.error());
    }
    for (const auto& item : page.value().items) {
        if (item.source_id == source_id) {
            return base::Result<contracts::SourceInventoryItem>::success(item);
        }
    }
    // Fallback: older inventory builds only emitted disk.N for empty disks. Resolve disk.N by
    // matching disk_number on any volume row so restore targets remain addressable.
    if (const auto disk_number = parse_disk_source_number(source_id)) {
        const contracts::SourceInventoryItem* first = nullptr;
        bool is_system = false;
        std::uint64_t disk_capacity = 0;
        for (const auto& item : page.value().items) {
            if (item.disk_number != *disk_number) {
                continue;
            }
            if (first == nullptr) {
                first = &item;
            }
            is_system = is_system || item.is_system;
            const auto item_disk = item.disk_capacity_bytes > 0 ? item.disk_capacity_bytes
                                                                : item.capacity_bytes;
            disk_capacity = (std::max)(disk_capacity, item_disk);
        }
        if (first != nullptr) {
            contracts::SourceInventoryItem synthetic = *first;
            synthetic.source_id = std::string(source_id);
            synthetic.display_name = "Disk " + std::to_string(*disk_number);
            synthetic.is_system = is_system;
            synthetic.disk_capacity_bytes = disk_capacity;
            return base::Result<contracts::SourceInventoryItem>::success(std::move(synthetic));
        }
    }
    return base::Result<contracts::SourceInventoryItem>::failure(
        {base::ErrorCode::kNotFound, "restore target was not found in inventory"});
}

struct DiskRestoreChainLayer final {
    std::string archive_key;
    std::string file_uuid;
};

struct DiskRestoreChain final {
    std::uint32_t source_disk_number{0};
    std::uint64_t disk_size_bytes{0};
    /// Base-first Full → … → tip.
    std::vector<DiskRestoreChainLayer> layers;
};

// Durable fingerprint (base-first):
// diskc|{source_disk}|{disk_size}|{depth}|{key0}|{uuid0}|…|{keyN-1}|{uuidN-1}
[[nodiscard]] std::string make_disk_restore_fingerprint(const DiskRestoreChain& chain) {
    std::string out = "diskc|" + std::to_string(chain.source_disk_number) + "|" +
                      std::to_string(chain.disk_size_bytes) + "|" +
                      std::to_string(chain.layers.size());
    for (const auto& layer : chain.layers) {
        out.push_back('|');
        out.append(layer.archive_key);
        out.push_back('|');
        out.append(layer.file_uuid);
    }
    return out;
}

[[nodiscard]] base::Result<DiskRestoreChain>
parse_disk_restore_fingerprint(const std::string_view fingerprint) {
    if (!fingerprint.starts_with("diskc|")) {
        return base::Result<DiskRestoreChain>::failure(
            {base::ErrorCode::kConflict, "restore preflight is not a disk restore"});
    }
    std::vector<std::string_view> parts;
    {
        std::size_t start = 0;
        while (start <= fingerprint.size()) {
            const auto bar = fingerprint.find('|', start);
            if (bar == std::string_view::npos) {
                parts.push_back(fingerprint.substr(start));
                break;
            }
            parts.push_back(fingerprint.substr(start, bar - start));
            start = bar + 1;
        }
    }
    // prefix + source_disk + disk_size + depth + 2*depth fields
    if (parts.size() < 4 || parts[0] != "diskc") {
        return base::Result<DiskRestoreChain>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    DiskRestoreChain parsed;
    {
        std::uint32_t source_disk = 0;
        const auto* begin = parts[1].data();
        const auto* end = begin + parts[1].size();
        if (std::from_chars(begin, end, source_disk).ec != std::errc{}) {
            return base::Result<DiskRestoreChain>::failure(
                {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
        }
        parsed.source_disk_number = source_disk;
    }
    {
        std::uint64_t disk_size = 0;
        const auto* begin = parts[2].data();
        const auto* end = begin + parts[2].size();
        if (std::from_chars(begin, end, disk_size).ec != std::errc{} || disk_size == 0) {
            return base::Result<DiskRestoreChain>::failure(
                {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
        }
        parsed.disk_size_bytes = disk_size;
    }
    std::uint32_t depth = 0;
    {
        const auto* begin = parts[3].data();
        const auto* end = begin + parts[3].size();
        if (std::from_chars(begin, end, depth).ec != std::errc{} || depth == 0) {
            return base::Result<DiskRestoreChain>::failure(
                {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
        }
    }
    if (parts.size() != 4U + static_cast<std::size_t>(depth) * 2U) {
        return base::Result<DiskRestoreChain>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    parsed.layers.reserve(depth);
    for (std::uint32_t index = 0; index < depth; ++index) {
        const auto& key = parts[4U + static_cast<std::size_t>(index) * 2U];
        const auto& uuid = parts[5U + static_cast<std::size_t>(index) * 2U];
        if (key.empty() || uuid.empty()) {
            return base::Result<DiskRestoreChain>::failure(
                {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
        }
        parsed.layers.push_back({std::string(key), std::string(uuid)});
    }
    return base::Result<DiskRestoreChain>::success(std::move(parsed));
}

[[nodiscard]] base::Result<std::uint64_t>
source_disk_size_from_archive(const std::string& archive_path_utf8,
                              const std::uint32_t source_disk_number,
                              const std::string& password) {
    auto path = path_from_utf8(archive_path_utf8);
    if (!path) {
        return base::Result<std::uint64_t>::failure(path.error());
    }
    adapters::personal_archive::ArchiveOpenRequest open_request;
    open_request.source = std::move(path).value();
    open_request.password = password;
    auto reader = adapters::personal_archive::PersonalArchiveReader::open(open_request);
    if (!reader) {
        return base::Result<std::uint64_t>::failure(reader.error());
    }
    for (const auto& disk : reader.value()->manifest().disks) {
        if (disk.disk_number == source_disk_number) {
            if (disk.disk_size == 0) {
                return base::Result<std::uint64_t>::failure(
                    {base::ErrorCode::kConflict, "source disk size is unavailable"});
            }
            return base::Result<std::uint64_t>::success(disk.disk_size);
        }
    }
    return base::Result<std::uint64_t>::failure(
        {base::ErrorCode::kNotFound, "source disk is not present in archive manifest"});
}

[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
load_catalog_entries(ports::IControlPlaneDatabase& control_plane,
                     ports::IRepositoryStorageFactory& storage_factory,
                     const std::string_view connection_id,
                     const base::CancellationToken cancellation) {
    auto repository = control_plane.get_repository_connection(connection_id, cancellation);
    if (!repository) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            repository.error());
    }
    if (!repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    auto storage = storage_factory.open(repository.value()->locator, cancellation);
    if (!storage) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            storage.error());
    }
    personal_repository::RepositoryCatalogScanner scanner(storage.value()->reader(),
                                                          storage.value()->enumerator());
    auto loaded = scanner.load_entries(cancellation);
    if (!loaded) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            loaded.error());
    }
    return base::Result<std::vector<personal_repository::CatalogEntry>>::success(
        std::move(loaded).value().entries);
}

[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
resolve_restore_chain_entries(ports::IControlPlaneDatabase& control_plane,
                              ports::IRepositoryStorageFactory& storage_factory,
                              const std::string_view connection_id,
                              const std::string_view recovery_point_id,
                              const base::CancellationToken cancellation) {
    auto entries =
        load_catalog_entries(control_plane, storage_factory, connection_id, cancellation);
    if (!entries) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            entries.error());
    }
    auto graph = personal_repository::RecoveryPointGraph::build(std::move(entries).value());
    if (!graph) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            graph.error());
    }
    auto chain = graph.value().resolve_chain(recovery_point_id);
    if (!chain) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(chain.error());
    }
    return base::Result<std::vector<personal_repository::CatalogEntry>>::success(
        std::move(chain).value());
}

[[nodiscard]] std::string restore_request_fingerprint(const std::string_view preflight_token) {
    return "start-restore|" + std::string(preflight_token);
}

WorkerJobService::WorkerJobService(application::ISourceInventoryQuery& source_inventory,
                                   ports::IControlPlaneDatabase& control_plane,
                                   ports::IRepositoryStorageFactory& storage_factory,
                                   WorkerSupervisor& supervisor, ports::IClock& clock,
                                   ports::IRandomSource& random) noexcept
    : source_inventory_(source_inventory), control_plane_(control_plane),
      storage_factory_(storage_factory), supervisor_(supervisor), clock_(clock), random_(random) {}

base::Result<contracts::CommandAcknowledgement>
WorkerJobService::start_backup(const contracts::StartBackupCommand& command,
                               const std::string_view idempotency_key,
                               const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_start_backup_command(command); !valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    if (command.backup_type == contracts::BackupType::kDifferential) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "differential backup is not available"});
    }
    auto plan = resolve_backup_plan(control_plane_, command, cancellation);
    if (!plan) {
        return base::Result<contracts::CommandAcknowledgement>::failure(plan.error());
    }
    auto existing = control_plane_.get_job_by_idempotency_key(idempotency_key, cancellation);
    if (!existing) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
    }
    if (existing.value()) {
        if (!same_backup(*existing.value(), plan.value())) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "idempotency key request mismatch"});
        }
        return base::Result<contracts::CommandAcknowledgement>::success(
            acknowledgement(existing.value()->job_id, contracts::CommandDisposition::kReplayed,
                            existing.value()->job_id));
    }
    PrepareBackupContext context{source_inventory_, control_plane_, storage_factory_, clock_,
                                 random_};
    auto prepared = prepare_backup(plan.value(), context, cancellation);
    if (!prepared) {
        return base::Result<contracts::CommandAcknowledgement>::failure(prepared.error());
    }
    prepared.value().request.idempotency_key = std::string(idempotency_key);
    auto submitted = supervisor_.submit(prepared.value().request, cancellation);
    if (!submitted) {
        return reconcile_submission_conflict(
            control_plane_, idempotency_key, plan.value(),
            [](const ports::JobRecord& record, const ResolvedBackupPlan& resolved) {
                return same_backup(record, resolved);
            },
            submitted.error(), cancellation);
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        acknowledgement(prepared.value().job_id, contracts::CommandDisposition::kAccepted,
                        prepared.value().job_id));
}

base::Result<contracts::CommandAcknowledgement>
WorkerJobService::start_verify(const contracts::StartVerifyCommand& command,
                               const std::string_view idempotency_key,
                               const base::CancellationToken cancellation) {
    auto valid = contracts::validate_start_verify_command(command);
    if (!valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    auto existing = control_plane_.get_job_by_idempotency_key(idempotency_key, cancellation);
    if (!existing) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
    }
    if (existing.value()) {
        if (!same_verify(*existing.value(), command)) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "idempotency key request mismatch"});
        }
        return base::Result<contracts::CommandAcknowledgement>::success(
            acknowledgement(existing.value()->job_id, contracts::CommandDisposition::kReplayed,
                            existing.value()->job_id));
    }
    auto prepared =
        prepare_verify(command, control_plane_, storage_factory_, random_, cancellation);
    if (!prepared) {
        return base::Result<contracts::CommandAcknowledgement>::failure(prepared.error());
    }
    prepared.value().request.idempotency_key = std::string(idempotency_key);
    auto submitted = supervisor_.submit(prepared.value().request, cancellation);
    if (!submitted) {
        return reconcile_submission_conflict(control_plane_, idempotency_key, command, same_verify,
                                             submitted.error(), cancellation);
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        acknowledgement(prepared.value().job_id, contracts::CommandDisposition::kAccepted,
                        prepared.value().job_id));
}

base::Result<contracts::RestorePreflight>
WorkerJobService::prepare_restore(const contracts::RestorePreflightRequest& request,
                                  const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_restore_preflight_request(request); !valid) {
        return base::Result<contracts::RestorePreflight>::failure(valid.error());
    }
    if (!is_disk_target_id(request.target_source_id)) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kConflict,
             "volume restore is not available; map a source disk to a disk.N target"});
    }
    auto target = find_inventory_item(source_inventory_, request.target_source_id, cancellation);
    if (!target) {
        return base::Result<contracts::RestorePreflight>::failure(target.error());
    }
    if (target.value().is_system) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kConflict, "system disk restore requires PE (not available online)"});
    }
    if (target.value().availability != contracts::SourceAvailability::kAvailable) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kConflict, "restore target is unavailable"});
    }
    const auto target_capacity = target.value().disk_capacity_bytes > 0
                                     ? target.value().disk_capacity_bytes
                                     : target.value().capacity_bytes;
    if (target_capacity == 0) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kConflict, "restore target capacity is unavailable"});
    }
    auto chain_entries = resolve_restore_chain_entries(
        control_plane_, storage_factory_, request.repository_connection_id,
        request.recovery_point_id, cancellation);
    if (!chain_entries) {
        return base::Result<contracts::RestorePreflight>::failure(chain_entries.error());
    }
    if (chain_entries.value().empty() ||
        chain_entries.value().back().file_uuid != request.recovery_point_id) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kConflict, "recovery point chain is incomplete"});
    }
    auto repository =
        control_plane_.get_repository_connection(request.repository_connection_id, cancellation);
    if (!repository || !repository.value()) {
        return base::Result<contracts::RestorePreflight>::failure(
            !repository ? repository.error()
                        : base::Error{base::ErrorCode::kNotFound, "repository connection not found"});
    }
    // Tip archive: geometry matches Full; opens with the supplied password for capacity check.
    auto tip_path = resolve_archive_absolute_path(repository.value()->locator,
                                                  chain_entries.value().back().archive_main_key);
    if (!tip_path) {
        return base::Result<contracts::RestorePreflight>::failure(tip_path.error());
    }
    auto disk_size = source_disk_size_from_archive(tip_path.value(), request.source_disk_number,
                                                   request.archive_password);
    if (!disk_size) {
        return base::Result<contracts::RestorePreflight>::failure(disk_size.error());
    }
    if (target_capacity < disk_size.value()) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kConflict, "restore target is smaller than the source disk"});
    }
    DiskRestoreChain chain;
    chain.source_disk_number = request.source_disk_number;
    chain.disk_size_bytes = disk_size.value();
    chain.layers.reserve(chain_entries.value().size());
    for (const auto& entry : chain_entries.value()) {
        chain.layers.push_back({entry.archive_main_key, entry.file_uuid});
    }
    const auto now = clock_.now_utc_ms();
    if (now < 0) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kInternal, "restore preflight clock is invalid"});
    }
    const auto now_u = static_cast<std::uint64_t>(now);
    if (now_u > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) -
                     kRestorePreflightTtlMs) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kInternal, "restore preflight clock is invalid"});
    }
    auto token = random_id("preflight-", random_, cancellation);
    if (!token) {
        return base::Result<contracts::RestorePreflight>::failure(token.error());
    }
    ports::RestorePreflightRecord record;
    record.preflight_token = token.value();
    record.repository_connection_id = request.repository_connection_id;
    record.repository_uuid = chain_entries.value().back().repository_uuid;
    record.recovery_point_id = request.recovery_point_id;
    record.target_source_id = request.target_source_id;
    record.chain_fingerprint = make_disk_restore_fingerprint(chain);
    record.logical_size_bytes = disk_size.value();
    record.target_capacity_bytes = target_capacity;
    record.chain_depth = static_cast<std::uint32_t>(chain.layers.size());
    record.created_utc_ms = now_u;
    record.expires_utc_ms = now_u + kRestorePreflightTtlMs;
    auto unit = control_plane_.begin_unit_of_work(cancellation);
    if (!unit) {
        return base::Result<contracts::RestorePreflight>::failure(unit.error());
    }
    if (auto inserted = unit.value()->restore_preflights().insert(record, cancellation);
        !inserted) {
        unit.value()->rollback();
        return base::Result<contracts::RestorePreflight>::failure(inserted.error());
    }
    if (auto committed = unit.value()->commit(cancellation); !committed) {
        return base::Result<contracts::RestorePreflight>::failure(committed.error());
    }
    contracts::RestorePreflight preflight;
    preflight.preflight_token = record.preflight_token;
    preflight.repository_connection_id = record.repository_connection_id;
    preflight.recovery_point_id = record.recovery_point_id;
    preflight.target_source_id = record.target_source_id;
    preflight.logical_size_bytes = record.logical_size_bytes;
    preflight.target_capacity_bytes = record.target_capacity_bytes;
    preflight.chain_depth = record.chain_depth;
    preflight.expires_utc_ms = record.expires_utc_ms;
    preflight.restore_eligible = true;
    preflight.message_code = "restore.preflight_ready";
    return base::Result<contracts::RestorePreflight>::success(std::move(preflight));
}

base::Result<contracts::CommandAcknowledgement>
WorkerJobService::start_restore(const contracts::StartRestoreCommand& command,
                                const std::string_view idempotency_key,
                                const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_start_restore_command(command); !valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    if (idempotency_key.empty()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kInvalidArgument, "idempotency key is required"});
    }
    auto existing = control_plane_.get_job_by_idempotency_key(idempotency_key, cancellation);
    if (!existing) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
    }
    const auto fingerprint = restore_request_fingerprint(command.preflight_token);
    if (existing.value()) {
        if (existing.value()->operation != contracts::JobOperation::kRestore ||
            existing.value()->request_fingerprint != fingerprint) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "idempotency key request mismatch"});
        }
        return base::Result<contracts::CommandAcknowledgement>::success(
            acknowledgement(existing.value()->job_id, contracts::CommandDisposition::kReplayed,
                            existing.value()->job_id));
    }
    auto preflight =
        control_plane_.get_restore_preflight(command.preflight_token, cancellation);
    if (!preflight) {
        return base::Result<contracts::CommandAcknowledgement>::failure(preflight.error());
    }
    if (!preflight.value()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kNotFound, "restore preflight was not found"});
    }
    const auto& record = *preflight.value();
    const auto now = static_cast<std::uint64_t>((std::max)(clock_.now_utc_ms(), 0LL));
    if (now >= record.expires_utc_ms) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "restore preflight has expired"});
    }
    auto parsed = parse_disk_restore_fingerprint(record.chain_fingerprint);
    if (!parsed) {
        return base::Result<contracts::CommandAcknowledgement>::failure(parsed.error());
    }
    if (parsed.value().layers.empty() ||
        parsed.value().layers.back().file_uuid != record.recovery_point_id) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    auto by_token = control_plane_.get_job_by_preflight_token(command.preflight_token, cancellation);
    if (!by_token) {
        return base::Result<contracts::CommandAcknowledgement>::failure(by_token.error());
    }
    if (by_token.value()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "restore preflight already started a job"});
    }
    auto target = find_inventory_item(source_inventory_, record.target_source_id, cancellation);
    if (!target) {
        return base::Result<contracts::CommandAcknowledgement>::failure(target.error());
    }
    if (target.value().is_system) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "system disk restore requires PE (not available online)"});
    }
    const auto target_capacity = target.value().disk_capacity_bytes > 0
                                     ? target.value().disk_capacity_bytes
                                     : target.value().capacity_bytes;
    if (target_capacity < parsed.value().disk_size_bytes) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "restore target is smaller than the source disk"});
    }
    auto repository =
        control_plane_.get_repository_connection(record.repository_connection_id, cancellation);
    if (!repository || !repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    // Re-resolve Catalog chain so deleted/moved parents fail before Worker launch.
    auto live_chain = resolve_restore_chain_entries(
        control_plane_, storage_factory_, record.repository_connection_id, record.recovery_point_id,
        cancellation);
    if (!live_chain) {
        return base::Result<contracts::CommandAcknowledgement>::failure(live_chain.error());
    }
    if (live_chain.value().size() != parsed.value().layers.size()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "restore chain changed after preflight"});
    }
    for (std::size_t index = 0; index < live_chain.value().size(); ++index) {
        if (live_chain.value()[index].file_uuid != parsed.value().layers[index].file_uuid ||
            live_chain.value()[index].archive_main_key !=
                parsed.value().layers[index].archive_key) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "restore chain changed after preflight"});
        }
    }
    std::vector<std::string> source_refs;
    source_refs.reserve(parsed.value().layers.size());
    for (const auto& layer : parsed.value().layers) {
        auto archive_path =
            resolve_archive_absolute_path(repository.value()->locator, layer.archive_key);
        if (!archive_path) {
            return base::Result<contracts::CommandAcknowledgement>::failure(archive_path.error());
        }
        source_refs.push_back(std::move(archive_path).value());
    }
    // Re-open tip to confirm password + source disk size still match preflight.
    auto disk_size = source_disk_size_from_archive(source_refs.back(),
                                                  parsed.value().source_disk_number,
                                                  command.archive_password);
    if (!disk_size) {
        return base::Result<contracts::CommandAcknowledgement>::failure(disk_size.error());
    }
    if (disk_size.value() != parsed.value().disk_size_bytes) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "restore source disk size changed after preflight"});
    }
    auto job_id = random_id("job-", random_, cancellation);
    auto trace_id = random_id("trace-", random_, cancellation);
    if (!job_id || !trace_id) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            !job_id ? job_id.error() : trace_id.error());
    }
    contracts::JobRequest worker;
    worker.job_id = job_id.value();
    worker.tenant_id = "personal";
    worker.operation = contracts::JobOperation::kRestore;
    worker.source_refs = std::move(source_refs);
    worker.target_ref =
        std::string(R"(\\.\PhysicalDrive)") + std::to_string(target.value().disk_number);
    if (command.archive_password.empty()) {
        worker.credential_refs.assign(worker.source_refs.size(), contracts::SecretRef{});
    } else {
        auto protected_secret = adapters::windows_system::protect_local_machine_secret(
            command.archive_password, job_id.value());
        if (!protected_secret) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                protected_secret.error());
        }
        // Personal Desktop supplies one password for the whole chain; replicate per layer.
        const contracts::SecretRef layer_secret = std::move(protected_secret).value();
        worker.credential_refs.assign(worker.source_refs.size(), layer_secret);
    }
    worker.trace_id = trace_id.value();
    contracts::RestoreOptions restore;
    restore.disk_restore = true;
    restore.source_disk_number = parsed.value().source_disk_number;
    restore.bring_target_online = true;
    restore.preserve_disk_signature = command.preserve_disk_signature;
    restore.auto_expand_last_partition = command.auto_expand_last_partition;
    worker.restore = std::move(restore);

    WorkerJobRequest request;
    request.worker_request = std::move(worker);
    request.source_ids = {record.recovery_point_id};
    request.repository_connection_id = record.repository_connection_id;
    request.idempotency_key = std::string(idempotency_key);
    request.request_fingerprint = fingerprint;
    request.preflight_token = command.preflight_token;
    request.target_source_id = record.target_source_id;
    auto submitted = supervisor_.submit(request, cancellation);
    if (!submitted) {
        return base::Result<contracts::CommandAcknowledgement>::failure(submitted.error());
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        acknowledgement(job_id.value(), contracts::CommandDisposition::kAccepted, job_id.value()));
}

base::Result<contracts::CommandAcknowledgement>
WorkerJobService::cancel_job(const contracts::ResourceRef& job,
                             const std::string_view idempotency_key,
                             const base::CancellationToken cancellation) {
    auto valid_job = contracts::validate_resource_ref(job);
    if (!valid_job) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid_job.error());
    }
    if (idempotency_key.empty()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kInvalidArgument, "idempotency key is required"});
    }
    const auto fingerprint = cancel_fingerprint(job.resource_id);
    auto prior = control_plane_.get_command(idempotency_key, cancellation);
    if (!prior)
        return base::Result<contracts::CommandAcknowledgement>::failure(prior.error());
    if (prior.value()) {
        if (prior.value()->request_fingerprint != fingerprint) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "idempotency key request mismatch"});
        }
        return base::Result<contracts::CommandAcknowledgement>::success(
            acknowledgement(prior.value()->command_id, contracts::CommandDisposition::kReplayed,
                            prior.value()->resource_id));
    }
    auto current = control_plane_.get_job(job.resource_id, cancellation);
    if (!current) {
        return base::Result<contracts::CommandAcknowledgement>::failure(current.error());
    }
    if (!current.value() || (current.value()->state != contracts::ServiceJobState::kRunning &&
                             current.value()->state != contracts::ServiceJobState::kCancelling)) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {current.value() ? base::ErrorCode::kConflict : base::ErrorCode::kNotFound,
             "job is not cancellable"});
    }
    auto result = persist_cancel_command(control_plane_, clock_, random_, *current.value(), job,
                                         idempotency_key, fingerprint, cancellation);
    if (!result)
        return result;
    auto signalled = supervisor_.cancel_job(job.resource_id, {});
    if (!signalled && signalled.error().code != base::ErrorCode::kNotFound) {
        return base::Result<contracts::CommandAcknowledgement>::failure(signalled.error());
    }
    return result;
}

} // namespace aegra::apps::service
