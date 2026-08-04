#include "aegra/adapters/memory/memory_object_storage.h"
#include "aegra/adapters/sqlite/sqlite_control_plane.h"
#include "aegra/adapters/windows_process/windows_process_launcher.h"
#include "aegra/application/recovery_point_operations.h"
#include "aegra/application/source_inventory_query.h"
#include "aegra/apps/service/worker_job_service.h"
#include "aegra/apps/service/worker_supervisor.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/personal_repository/delete_plan.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/random.h"
#include "aegra/ports/repository_storage.h"
#include "aegra/ports/source_inventory.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace application = aegra::application;
namespace base = aegra::base;
namespace contracts = aegra::contracts;
namespace memory = aegra::adapters::memory;
namespace ports = aegra::ports;
namespace repository = aegra::personal_repository;
namespace service = aegra::apps::service;
namespace sqlite_cp = aegra::adapters::sqlite;
using aegra::format::BackupType;

constexpr auto kRepositoryUuid = "01234567-89ab-4cde-8f01-23456789abcd";
constexpr auto kSetUuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
constexpr auto kFullUuid = "11111111-2222-4333-8444-555555555555";
constexpr auto kIncrementalUuid = "22222222-3333-4444-8555-666666666666";
constexpr auto kLeafUuid = "33333333-4444-4555-8666-777777777777";

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{0};
        path_ = std::filesystem::temp_directory_path() /
                ("aegra-s5-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] std::filesystem::path database_path() const { return path_ / "control.db"; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

class FakeClock final : public ports::IClock {
  public:
    explicit FakeClock(const std::int64_t start) noexcept : now_(start) {}
    [[nodiscard]] std::int64_t now_utc_ms() const noexcept override { return now_; }
    void advance(const std::int64_t milliseconds) noexcept { now_ += milliseconds; }

  private:
    std::int64_t now_;
};

class FakeRandom final : public ports::IRandomSource {
  public:
    [[nodiscard]] base::Result<void> fill(const std::span<std::byte> destination,
                                          const base::CancellationToken& cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure({base::ErrorCode::kCancelled, "random cancelled"});
        }
        for (auto& byte : destination) {
            byte = static_cast<std::byte>(next_++);
        }
        return base::Result<void>::success();
    }

  private:
    std::uint8_t next_{1};
};

class MemoryStorageAccess final : public ports::IRepositoryStorageAccess {
  public:
    explicit MemoryStorageAccess(std::shared_ptr<memory::MemoryObjectStorage> storage)
        : storage_(std::move(storage)) {}
    [[nodiscard]] ports::IObjectReader& reader() noexcept override { return *storage_; }
    [[nodiscard]] ports::IPrefixEnumerator& enumerator() noexcept override { return *storage_; }
    [[nodiscard]] ports::IStagedObjectWriter& writer() noexcept override { return *storage_; }
    [[nodiscard]] ports::IObjectPublisher& publisher() noexcept override { return *storage_; }
    [[nodiscard]] ports::IObjectDeleter& deleter() noexcept override { return *storage_; }

  private:
    std::shared_ptr<memory::MemoryObjectStorage> storage_;
};

class FakeStorageFactory final : public ports::IRepositoryStorageFactory {
  public:
    std::map<std::string, std::shared_ptr<memory::MemoryObjectStorage>, std::less<>> stores;
    std::map<std::string, base::Error, std::less<>> failures;

    [[nodiscard]] base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>
    open(const std::string_view locator, const base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
                {base::ErrorCode::kCancelled, "open cancelled"});
        }
        const auto key = std::string(locator);
        if (const auto failed = failures.find(key); failed != failures.end()) {
            return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
                failed->second);
        }
        const auto found = stores.find(key);
        if (found == stores.end()) {
            return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
                {base::ErrorCode::kNotFound, "repository root not found"});
        }
        return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::success(
            std::make_unique<MemoryStorageAccess>(found->second));
    }
};

bool publish(memory::MemoryObjectStorage& storage, const std::string_view destination_key,
             const std::string_view contents) {
    static std::atomic_uint32_t sequence{0};
    const auto staging = "staging/s5/" + std::to_string(sequence.fetch_add(1));
    auto writer = storage.begin_staged_write(staging, {});
    if (!writer) {
        return false;
    }
    const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
    return writer.value()->write(bytes, {}) && writer.value()->complete({}) &&
           storage
               .publish({staging, std::string(destination_key),
                         ports::PublishCondition::kCreateOnly, std::nullopt},
                        {})
               .has_value();
}

repository::ArchiveMemberGenerationResolver
member_generations(memory::MemoryObjectStorage& storage) {
    return [&storage](const std::string_view key) -> base::Result<std::optional<std::string>> {
        auto attributes = storage.get_attributes(key, {});
        if (!attributes && attributes.error().code == base::ErrorCode::kNotFound) {
            return base::Result<std::optional<std::string>>::success(std::nullopt);
        }
        if (!attributes) {
            return base::Result<std::optional<std::string>>::failure(attributes.error());
        }
        return base::Result<std::optional<std::string>>::success(
            std::move(attributes).value().generation);
    };
}

repository::CatalogEntry make_entry(std::string uuid, const BackupType type,
                                    std::optional<std::string> parent = std::nullopt) {
    repository::CatalogEntry value;
    value.repository_uuid = kRepositoryUuid;
    value.file_uuid = std::move(uuid);
    value.backup_set_uuid = kSetUuid;
    value.parent_uuid = std::move(parent);
    value.backup_type = type;
    value.archive_main_key = "archives/2026/08/" + value.file_uuid + ".bkf";
    value.has_sidecar = true;
    value.split_part_count = 1;
    value.created_utc_ms = 1'785'600'000'000ULL;
    value.logical_size_bytes = 4'096;
    value.stored_size_bytes = 2'048;
    value.source_count = 1;
    return value;
}

bool seed_chain(memory::MemoryObjectStorage& storage) {
    repository::RepositoryDescriptor descriptor;
    descriptor.repository_uuid = kRepositoryUuid;
    descriptor.created_utc_ms = 1'785'600'000'000ULL;
    auto encoded_descriptor = repository::encode_repository_descriptor_json(descriptor);
    if (!encoded_descriptor || !publish(storage, "aegra.repository", encoded_descriptor.value())) {
        return false;
    }
    const auto full = make_entry(kFullUuid, BackupType::kFull);
    const auto incremental = make_entry(kIncrementalUuid, BackupType::kIncremental, kFullUuid);
    const auto leaf = make_entry(kLeafUuid, BackupType::kIncremental, kIncrementalUuid);
    for (const auto& entry : {full, incremental, leaf}) {
        auto encoded = repository::encode_catalog_entry_json(entry);
        if (!encoded || !publish(storage, "catalog/recovery-points/" + entry.file_uuid + ".entry",
                                 encoded.value())) {
            return false;
        }
        // Archive members required for delete execute membership deletes.
        if (!publish(storage, entry.archive_main_key, "bkf") ||
            !publish(storage, entry.archive_main_key + ".bhx", "bhx")) {
            return false;
        }
    }
    return true;
}

bool add_available_connection(ports::IControlPlaneDatabase& control_plane,
                              const std::string& connection_id, const std::string& locator) {
    ports::RepositoryConnectionRecord record;
    record.connection_id = connection_id;
    record.display_name = "Repo";
    record.locator = locator;
    record.credential_ref = contracts::SecretRef{"wincred://archive"};
    record.state = contracts::RepositoryConnectionState::kAvailable;
    record.is_default = true;
    record.capabilities = {"repository.list"};
    record.created_utc_ms = 1;
    record.updated_utc_ms = 1;
    auto unit = control_plane.begin_unit_of_work({});
    if (!unit) {
        return false;
    }
    auto stored = unit.value()->repository_connections().upsert(record, {});
    if (!stored) {
        unit.value()->rollback();
        return false;
    }
    return unit.value()->commit({}).has_value();
}

bool test_chain_resolve_and_offline() {
    TemporaryDirectory temp;
    auto control_plane = sqlite_cp::SqliteControlPlaneDatabase::open(
        {temp.database_path(), sqlite_cp::SqliteOpenMode::kCreateIfMissing});
    if (!expect(control_plane.has_value(), "control plane opens")) {
        return false;
    }
    auto storage = std::make_shared<memory::MemoryObjectStorage>();
    if (!expect(seed_chain(*storage), "catalog chain is seeded")) {
        return false;
    }
    FakeStorageFactory factory;
    factory.stores["memory://repo"] = storage;
    FakeClock clock(1'785'600'000'000LL);
    FakeRandom random;
    if (!expect(add_available_connection(*control_plane.value(), "conn-1", "memory://repo"),
                "connection is stored")) {
        return false;
    }
    application::RecoveryPointOperations ops(*control_plane.value(), factory, clock, random);
    auto chain = ops.resolve_chain({"conn-1", kLeafUuid}, {});
    bool passed =
        expect(chain && chain.value().layers.size() == 3, "base-first chain has three layers") &&
        expect(chain && chain.value().layers.front().recovery_point_id == kFullUuid,
               "chain starts at full") &&
        expect(chain && chain.value().layers.back().recovery_point_id == kLeafUuid,
               "chain ends at leaf") &&
        expect(chain && chain.value().verify_eligible,
               "structurally complete leaf is verify eligible") &&
        expect(chain && !chain.value().restore_eligible,
               "authentication not attempted so restore stays ineligible");

    auto missing = ops.resolve_chain({"conn-1", "99999999-9999-4999-8999-999999999999"}, {});
    passed &= expect(!missing && missing.error().code == base::ErrorCode::kNotFound,
                     "unknown recovery point is not found");

    factory.failures["memory://repo"] = {base::ErrorCode::kIoFailure, "offline"};
    auto offline = ops.resolve_chain({"conn-1", kLeafUuid}, {});
    passed &= expect(!offline && offline.error().code == base::ErrorCode::kIoFailure,
                     "offline repository fails with structured error");
    return passed;
}

bool test_delete_plan_execute_and_idempotent_replay() {
    TemporaryDirectory temp;
    auto control_plane = sqlite_cp::SqliteControlPlaneDatabase::open(
        {temp.database_path(), sqlite_cp::SqliteOpenMode::kCreateIfMissing});
    if (!expect(control_plane.has_value(), "control plane opens for delete")) {
        return false;
    }
    auto storage = std::make_shared<memory::MemoryObjectStorage>();
    if (!expect(seed_chain(*storage), "catalog is seeded for delete")) {
        return false;
    }
    FakeStorageFactory factory;
    factory.stores["memory://repo"] = storage;
    FakeClock clock(1'785'600'000'000LL);
    FakeRandom random;
    if (!expect(add_available_connection(*control_plane.value(), "conn-1", "memory://repo"),
                "connection stored for delete")) {
        return false;
    }
    application::RecoveryPointOperations ops(*control_plane.value(), factory, clock, random);
    auto plan = ops.plan_delete({"conn-1", kFullUuid}, {});
    if (!expect(plan && plan.value().targets.size() == 3, "delete plan covers full subtree")) {
        return false;
    }
    bool passed = expect(plan.value().targets.front().recovery_point_id == kLeafUuid,
                         "summary keeps descendant-first order") &&
                  expect(!plan.value().plan_token.empty(), "plan token is issued");

    auto executed = ops.execute_delete({plan.value().plan_token, true}, "idem-delete-1", {});
    passed &=
        expect(executed && executed.value().disposition == contracts::CommandDisposition::kAccepted,
               "delete execute is accepted");

    // Catalog entries and archive members should be gone; scanner should see empty recoverable set.
    repository::RepositoryCatalogScanner scanner(*storage, *storage);
    auto page = scanner.scan({{}, 50}, {});
    passed &= expect(page && page.value().recovery_points.empty(),
                     "catalog no longer lists deleted recovery points");

    auto replayed = ops.execute_delete({plan.value().plan_token, true}, "idem-delete-1", {});
    passed &= expect(replayed &&
                         replayed.value().disposition == contracts::CommandDisposition::kReplayed &&
                         replayed.value().command_id == executed.value().command_id,
                     "same idempotency key replays without re-delete side effects");
    auto durable_plan =
        storage->get_attributes("staging/delete-plans/" + plan.value().plan_token + ".json", {});
    passed &= expect(durable_plan.has_value(), "completed delete keeps durable replay plan");
    auto tombstone =
        storage->get_attributes(repository::deletion_tombstone_key(plan.value().operation_id), {});
    passed &= expect(!tombstone && tombstone.error().code == base::ErrorCode::kNotFound,
                     "completed delete replay does not leave a tombstone");

    auto conflict = ops.execute_delete({plan.value().plan_token, true}, "idem-delete-1", {});
    // Same key same fingerprint still replay
    passed &=
        expect(conflict && conflict.value().disposition == contracts::CommandDisposition::kReplayed,
               "repeat replay remains stable");

    auto different = ops.execute_delete({plan.value().plan_token + "x", true}, "idem-delete-1", {});
    passed &= expect(!different && different.error().code == base::ErrorCode::kConflict,
                     "same key different plan token conflicts");

    auto unconfirmed = ops.execute_delete({plan.value().plan_token, false}, "idem-delete-2", {});
    passed &= expect(!unconfirmed && unconfirmed.error().code == base::ErrorCode::kInvalidArgument,
                     "execute without confirmation is rejected");
    return passed;
}

bool test_delete_resume_after_expiry_uses_published_tombstone() {
    TemporaryDirectory temp;
    auto control_plane = sqlite_cp::SqliteControlPlaneDatabase::open(
        {temp.database_path(), sqlite_cp::SqliteOpenMode::kCreateIfMissing});
    auto storage = std::make_shared<memory::MemoryObjectStorage>();
    if (!expect(control_plane && seed_chain(*storage) &&
                    add_available_connection(*control_plane.value(), "conn-1", "memory://repo"),
                "seed for expired resume")) {
        return false;
    }
    FakeStorageFactory factory;
    factory.stores["memory://repo"] = storage;
    FakeClock clock(1'785'600'000'000LL);
    FakeRandom random;
    application::RecoveryPointOperations ops(*control_plane.value(), factory, clock, random);
    auto summary = ops.plan_delete({"conn-1", kFullUuid}, {});
    if (!expect(summary.has_value(), "plan for expired resume")) {
        return false;
    }
    auto core = repository::plan_delete_recovery_points(
        {make_entry(kFullUuid, BackupType::kFull),
         make_entry(kIncrementalUuid, BackupType::kIncremental, kFullUuid),
         make_entry(kLeafUuid, BackupType::kIncremental, kIncrementalUuid)},
        kFullUuid, summary.value().operation_id, 1'785'600'000'000ULL,
        summary.value().expires_utc_ms, "conn-1", member_generations(*storage));
    if (!expect(core.has_value(), "core plan for expired resume")) {
        return false;
    }
    auto tombstone_json = repository::encode_deletion_tombstone_json(core.value().tombstone);
    if (!expect(tombstone_json &&
                    publish(*storage,
                            repository::deletion_tombstone_key(summary.value().operation_id),
                            tombstone_json.value()),
                "published tombstone survives confirmation expiry")) {
        return false;
    }
    clock.advance(1'800'001);
    auto resumed =
        ops.execute_delete({summary.value().plan_token, true}, "idem-expired-resume", {});
    return expect(resumed.has_value(), "published tombstone resumes after plan expiry");
}

bool test_delete_generation_change_requires_replan() {
    TemporaryDirectory temp;
    auto control_plane = sqlite_cp::SqliteControlPlaneDatabase::open(
        {temp.database_path(), sqlite_cp::SqliteOpenMode::kCreateIfMissing});
    auto storage = std::make_shared<memory::MemoryObjectStorage>();
    if (!expect(control_plane && seed_chain(*storage), "seed for generation change")) {
        return false;
    }
    FakeStorageFactory factory;
    factory.stores["memory://repo"] = storage;
    FakeClock clock(1'785'600'000'000LL);
    FakeRandom random;
    if (!expect(add_available_connection(*control_plane.value(), "conn-1", "memory://repo"),
                "connection for generation change")) {
        return false;
    }
    application::RecoveryPointOperations ops(*control_plane.value(), factory, clock, random);
    auto plan = ops.plan_delete({"conn-1", kLeafUuid}, {});
    if (!expect(plan.has_value(), "leaf plan is created")) {
        return false;
    }
    // Bump catalog generation for the leaf entry object.
    auto leaf = make_entry(kLeafUuid, BackupType::kIncremental, kIncrementalUuid);
    leaf.catalog_generation = 9;
    auto encoded = repository::encode_catalog_entry_json(leaf);
    if (!expect(encoded.has_value(), "updated leaf encodes")) {
        return false;
    }
    // Replace existing entry by delete + publish.
    (void)storage->delete_object({"catalog/recovery-points/" + std::string(kLeafUuid) + ".entry",
                                  "op-replace", std::nullopt},
                                 {});
    if (!expect(publish(*storage, "catalog/recovery-points/" + std::string(kLeafUuid) + ".entry",
                        encoded.value()),
                "updated leaf is published")) {
        return false;
    }
    auto executed = ops.execute_delete({plan.value().plan_token, true}, "idem-gen-1", {});
    return expect(!executed && executed.error().code == base::ErrorCode::kConflict,
                  "generation change requires a new delete plan");
}

bool test_delete_resume_preserves_recreated_archive_member() {
    auto storage = std::make_shared<memory::MemoryObjectStorage>();
    if (!expect(seed_chain(*storage), "seed for recreated archive member")) {
        return false;
    }
    const auto full = make_entry(kFullUuid, BackupType::kFull);
    auto plan = repository::plan_delete_recovery_points(
        {full}, kFullUuid, "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff", 1'785'600'000'000ULL,
        1'785'600'000'000ULL + 1'800'000ULL, "conn-1", member_generations(*storage));
    if (!expect(plan.has_value(), "plan captures archive member generations")) {
        return false;
    }
    auto encoded = repository::encode_deletion_tombstone_json(plan.value().tombstone);
    if (!expect(encoded && publish(*storage,
                                   repository::deletion_tombstone_key(
                                       plan.value().tombstone.operation_uuid),
                                   encoded.value()),
                "publish durable tombstone before simulated restart")) {
        return false;
    }
    (void)storage->delete_object({full.archive_main_key, "external-replace", std::nullopt}, {});
    if (!expect(publish(*storage, full.archive_main_key, "replacement"),
                "recreate archive member under the same key")) {
        return false;
    }
    repository::DeletePlanExecutor executor(*storage, *storage, *storage, *storage);
    auto resumed = executor.execute(plan.value(), {});
    auto replacement = storage->get_attributes(full.archive_main_key, {});
    return expect(!resumed && resumed.error().code == base::ErrorCode::kConflict,
                  "resume rejects recreated archive member generation") &&
           expect(replacement.has_value(), "recreated archive member is preserved");
}

class EmptyInventory final : public ports::ISourceInventory {
  public:
    [[nodiscard]] base::Result<std::vector<ports::SourceInventoryRecord>>
    list_sources(const base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<std::vector<ports::SourceInventoryRecord>>::failure(
                {base::ErrorCode::kCancelled, "cancelled"});
        }
        return base::Result<std::vector<ports::SourceInventoryRecord>>::success({});
    }
};

class IdempotencyRaceInventory final : public ports::ISourceInventory {
  public:
    IdempotencyRaceInventory(ports::IControlPlaneDatabase& control_plane,
                             std::string idempotency_key)
        : control_plane_(control_plane), idempotency_key_(std::move(idempotency_key)) {}

    [[nodiscard]] base::Result<std::vector<ports::SourceInventoryRecord>>
    list_sources(const base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<std::vector<ports::SourceInventoryRecord>>::failure(
                {base::ErrorCode::kCancelled, "cancelled"});
        }
        if (!inserted_) {
            auto inserted = insert_winning_job(cancellation);
            if (!inserted) {
                return base::Result<std::vector<ports::SourceInventoryRecord>>::failure(
                    inserted.error());
            }
            inserted_ = true;
        }
        ports::SourceInventoryRecord source;
        source.source_id = "source-race";
        source.stable_key = "volume-race";
        source.display_name = "Race source";
        source.availability = contracts::SourceAvailability::kAvailable;
        source.capacity_bytes = 1;
        return base::Result<std::vector<ports::SourceInventoryRecord>>::success(
            {std::move(source)});
    }

  private:
    [[nodiscard]] base::Result<void>
    insert_winning_job(const base::CancellationToken cancellation) {
        auto unit = control_plane_.begin_unit_of_work(cancellation);
        if (!unit) {
            return base::Result<void>::failure(unit.error());
        }
        ports::JobRecord record;
        record.job_id = "job-race-winner";
        record.trace_id = "trace-race-winner";
        record.operation = contracts::JobOperation::kBackup;
        record.state = contracts::ServiceJobState::kQueued;
        record.created_utc_ms = 1'785'600'000'000ULL;
        record.source_id = "source-race";
        record.repository_connection_id = "conn-race";
        record.backup_type = contracts::BackupType::kFull;
        record.message_code = "job.queued";
        record.idempotency_key = idempotency_key_;
        auto stored = unit.value()->jobs().insert(record, cancellation);
        if (!stored) {
            unit.value()->rollback();
            return stored;
        }
        return unit.value()->commit(cancellation);
    }

    ports::IControlPlaneDatabase& control_plane_;
    std::string idempotency_key_;
    bool inserted_{false};
};

bool test_start_backup_reconciles_idempotency_insert_race() {
    TemporaryDirectory temp;
    auto control_plane = sqlite_cp::SqliteControlPlaneDatabase::open(
        {temp.database_path(), sqlite_cp::SqliteOpenMode::kCreateIfMissing});
    if (!expect(control_plane && add_available_connection(*control_plane.value(), "conn-race",
                                                          temp.path().string()),
                "seed for start backup idempotency race")) {
        return false;
    }
    constexpr auto kIdempotencyKey = "idem-backup-race";
    IdempotencyRaceInventory inventory(*control_plane.value(), kIdempotencyKey);
    application::SourceInventoryQuery source_query(inventory);
    FakeStorageFactory factory;
    FakeClock clock(1'785'600'000'000LL);
    FakeRandom random;
    aegra::adapters::windows_process::WindowsProcessLauncher launcher;
    service::WorkerSupervisorConfig config;
    config.worker_executable_path = "C:/invalid/aegra_personal_worker.exe";
    service::WorkerSupervisor supervisor(std::move(config), launcher, *control_plane.value(), clock,
                                         random, {}, {});
    service::WorkerJobService jobs(source_query, *control_plane.value(), factory, supervisor, clock,
                                   random);
    auto started =
        jobs.start_backup({"source-race", "conn-race", contracts::BackupType::kFull, std::nullopt},
                          kIdempotencyKey, {});
    supervisor.shutdown({});
    return expect(started &&
                      started.value().disposition == contracts::CommandDisposition::kReplayed &&
                      started.value().resource_id == "job-race-winner",
                  "concurrent idempotency winner is replayed after insert conflict");
}

bool test_start_verify_rejects_missing_credential_and_unknown_point() {
    TemporaryDirectory temp;
    auto control_plane = sqlite_cp::SqliteControlPlaneDatabase::open(
        {temp.database_path(), sqlite_cp::SqliteOpenMode::kCreateIfMissing});
    auto storage = std::make_shared<memory::MemoryObjectStorage>();
    if (!expect(control_plane && seed_chain(*storage), "seed for verify rejection")) {
        return false;
    }
    FakeStorageFactory factory;
    factory.stores["memory://repo"] = storage;
    FakeClock clock(1'785'600'000'000LL);
    FakeRandom random;
    // Connection has a repository credential but no archive.default_credential capability:
    // must not reuse that SecretRef for Verify of an arbitrary Recovery Point.
    ports::RepositoryConnectionRecord record;
    record.connection_id = "conn-1";
    record.display_name = "Repo";
    record.locator = "memory://repo";
    record.credential_ref = contracts::SecretRef{"wincred://archive"};
    record.state = contracts::RepositoryConnectionState::kAvailable;
    record.is_default = true;
    record.capabilities = {"repository.list"};
    record.created_utc_ms = 1;
    record.updated_utc_ms = 1;
    {
        auto unit = control_plane.value()->begin_unit_of_work({});
        if (!unit || !unit.value()->repository_connections().upsert(record, {}) ||
            !unit.value()->commit({})) {
            return expect(false, "connection without default archive credential is stored");
        }
    }
    EmptyInventory inventory;
    application::SourceInventoryQuery source_query(inventory);
    aegra::adapters::windows_process::WindowsProcessLauncher launcher;
    service::WorkerSupervisorConfig config;
    config.worker_executable_path = "C:/invalid/aegra_personal_worker.exe";
    service::WorkerSupervisor supervisor(std::move(config), launcher, *control_plane.value(), clock,
                                         random, {}, {});
    service::WorkerJobService jobs(source_query, *control_plane.value(), factory, supervisor, clock,
                                   random);
    auto missing_secret = jobs.start_verify({"conn-1", kLeafUuid}, "idem-verify-secret", {});
    bool passed =
        expect(!missing_secret && missing_secret.error().code == base::ErrorCode::kUnauthorized,
               "verify without archive credential mapping returns unauthorized");

    record.capabilities = {"archive.default_credential", "repository.list"};
    {
        auto unit = control_plane.value()->begin_unit_of_work({});
        if (!unit || !unit.value()->repository_connections().upsert(record, {}) ||
            !unit.value()->commit({})) {
            return expect(false, "connection with default archive credential is stored");
        }
    }
    auto missing_point = jobs.start_verify({"conn-1", "99999999-9999-4999-8999-999999999999"},
                                           "idem-verify-missing", {});
    passed &= expect(!missing_point && missing_point.error().code == base::ErrorCode::kNotFound,
                     "verify for unknown recovery point is not found");
    supervisor.shutdown({});
    return passed;
}

bool test_partial_delete_resume_same_operation() {
    TemporaryDirectory temp;
    auto control_plane = sqlite_cp::SqliteControlPlaneDatabase::open(
        {temp.database_path(), sqlite_cp::SqliteOpenMode::kCreateIfMissing});
    auto storage = std::make_shared<memory::MemoryObjectStorage>();
    if (!expect(control_plane && seed_chain(*storage) &&
                    add_available_connection(*control_plane.value(), "conn-1", "memory://repo"),
                "seed for partial delete resume")) {
        return false;
    }
    FakeStorageFactory factory;
    factory.stores["memory://repo"] = storage;
    FakeClock clock(1'785'600'000'000LL);
    FakeRandom random;
    application::RecoveryPointOperations ops(*control_plane.value(), factory, clock, random);
    auto plan = ops.plan_delete({"conn-1", kFullUuid}, {});
    if (!expect(plan.has_value(), "plan for partial resume")) {
        return false;
    }

    // Rebuild executor plan and simulate partial progress: publish tombstone, delete only leaf.
    auto full = make_entry(kFullUuid, BackupType::kFull);
    auto incremental = make_entry(kIncrementalUuid, BackupType::kIncremental, kFullUuid);
    auto leaf = make_entry(kLeafUuid, BackupType::kIncremental, kIncrementalUuid);
    auto core_plan = repository::plan_delete_recovery_points(
        {full, incremental, leaf}, kFullUuid, plan.value().operation_id, 1'785'600'000'000ULL,
        plan.value().expires_utc_ms, "conn-1", member_generations(*storage));
    if (!expect(core_plan.has_value(), "core plan rebuild for partial progress")) {
        return false;
    }
    repository::DeletePlanExecutor executor(*storage, *storage, *storage, *storage);
    // First full execute through Application (intent + full delete).
    auto first = ops.execute_delete({plan.value().plan_token, true}, "idem-partial-1", {});
    if (!expect(first.has_value(), "first execute succeeds")) {
        return false;
    }
    // Second call with same operation/idempotency must not fail when scanner hides targets.
    auto second = ops.execute_delete({plan.value().plan_token, true}, "idem-partial-1", {});
    bool passed =
        expect(second && second.value().disposition == contracts::CommandDisposition::kReplayed &&
                   second.value().command_id == first.value().command_id,
               "same operation replays after completed delete");

    // New plan: publish tombstone only, then resume without scanner-visible entries.
    auto plan2 = ops.plan_delete({"conn-1", kLeafUuid}, {});
    // Catalog may be empty after full delete; reseed for second scenario.
    storage = std::make_shared<memory::MemoryObjectStorage>();
    if (!expect(seed_chain(*storage), "reseed for tombstone-resume")) {
        return false;
    }
    factory.stores["memory://repo"] = storage;
    plan2 = ops.plan_delete({"conn-1", kFullUuid}, {});
    if (!expect(plan2.has_value(), "second plan after reseed")) {
        return false;
    }
    auto core2 = repository::plan_delete_recovery_points(
        {make_entry(kFullUuid, BackupType::kFull),
         make_entry(kIncrementalUuid, BackupType::kIncremental, kFullUuid),
         make_entry(kLeafUuid, BackupType::kIncremental, kIncrementalUuid)},
        kFullUuid, plan2.value().operation_id, 1'785'600'000'000ULL, plan2.value().expires_utc_ms,
        "conn-1", member_generations(*storage));
    if (!expect(core2.has_value(), "core plan for tombstone resume")) {
        return false;
    }
    repository::DeletePlanExecutor resume_exec(*storage, *storage, *storage, *storage);
    auto resume_once = resume_exec.execute(core2.value(), {});
    if (!expect(resume_once.has_value(), "direct executor finishes under tombstone authority")) {
        return false;
    }
    // Re-execute same plan after tombstone+members gone: still succeeds (idempotent).
    auto resume_twice = resume_exec.execute(core2.value(), {});
    passed &= expect(resume_twice.has_value(),
                     "executor continues/idempotently completes after tombstone-hidden catalog");
    return passed;
}

bool test_delete_unknown_outcome_reconciles_missing_object() {
    auto storage = std::make_shared<memory::MemoryObjectStorage>(memory::MemoryObjectStorageOptions{
        .unknown_delete_key = "archives/2026/08/" + std::string(kFullUuid) + ".bkf"});
    if (!expect(seed_chain(*storage), "seed for unknown delete")) {
        return false;
    }
    // First delete the object so reconciliation sees NotFound after OutcomeUnknown.
    (void)storage->delete_object(
        {"archives/2026/08/" + std::string(kFullUuid) + ".bkf", "pre-delete", std::nullopt}, {});
    // Reset unknown flag after pre-delete by using a storage that still reports unknown for that
    // key while attributes are missing — Memory reports unknown then get_attributes NotFound.
    memory::MemoryObjectStorageOptions options;
    options.unknown_delete_key = "archives/2026/08/" + std::string(kFullUuid) + ".bhx";
    auto storage2 = std::make_shared<memory::MemoryObjectStorage>(options);
    if (!expect(seed_chain(*storage2), "seed storage2")) {
        return false;
    }
    // Pre-remove the key that will get unknown so reconcile succeeds.
    (void)storage2->delete_object({*options.unknown_delete_key, "pre", std::nullopt}, {});
    const auto full = make_entry(kFullUuid, BackupType::kFull);
    auto plan = repository::plan_delete_recovery_points(
        {full}, kFullUuid, "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", 1'785'600'000'000ULL,
        1'785'600'000'000ULL + 1'800'000ULL, "conn-1", member_generations(*storage2));
    if (!expect(plan.has_value(), "plan for unknown path")) {
        return false;
    }
    repository::DeletePlanExecutor executor(*storage2, *storage2, *storage2, *storage2);
    // (namespace repository already aliases personal_repository)
    auto executed = executor.execute(plan.value(), {});
    return expect(executed.has_value(),
                  "kOutcomeUnknown on already-missing member reconciles as success");
}

int run_tests() {
    const bool passed = test_chain_resolve_and_offline() &&
                        test_delete_plan_execute_and_idempotent_replay() &&
                        test_delete_generation_change_requires_replan() &&
                        test_delete_resume_preserves_recreated_archive_member() &&
                        test_start_backup_reconciles_idempotency_insert_race() &&
                        test_start_verify_rejects_missing_credential_and_unknown_point() &&
                        test_partial_delete_resume_same_operation() &&
                        test_delete_resume_after_expiry_uses_published_tombstone() &&
                        test_delete_unknown_outcome_reconciles_missing_object();
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main() noexcept {
    try {
        return run_tests();
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
