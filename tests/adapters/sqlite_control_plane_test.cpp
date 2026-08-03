#include "aegra/adapters/sqlite/sqlite_control_plane.h"
#include "aegra/base/error.h"
#include "aegra/ports/control_plane.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

namespace sqlite_cp = aegra::adapters::sqlite;
namespace ports = aegra::ports;
namespace contracts = aegra::contracts;
namespace base = aegra::base;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{0};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("aegra-sqlite-cp-" + std::to_string(timestamp) + "-" +
                 std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path database_path() const { return path_ / "control.db"; }

  private:
    std::filesystem::path path_;
};

bool expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] base::Result<std::unique_ptr<sqlite_cp::SqliteControlPlaneDatabase>>
open_db(const std::filesystem::path& path,
        const sqlite_cp::SqliteOpenMode mode = sqlite_cp::SqliteOpenMode::kCreateIfMissing) {
    return sqlite_cp::SqliteControlPlaneDatabase::open({path, mode});
}

ports::RepositoryConnectionRecord make_connection(const std::string& id, const std::string& locator,
                                                  const bool is_default, const std::uint64_t utc) {
    ports::RepositoryConnectionRecord record;
    record.connection_id = id;
    record.display_name = "Repo " + id;
    record.locator = locator;
    record.credential_ref = contracts::SecretRef{"wincred://repo-" + id};
    record.state = contracts::RepositoryConnectionState::kAvailable;
    record.is_default = is_default;
    record.capabilities = {"repository.list"};
    record.created_utc_ms = utc;
    record.updated_utc_ms = utc;
    return record;
}

ports::JobRecord make_job(const std::string& id, const contracts::ServiceJobState state,
                          const std::uint64_t created,
                          const std::optional<std::string>& connection) {
    ports::JobRecord record;
    record.job_id = id;
    record.trace_id = "trace-" + id;
    record.operation = contracts::JobOperation::kBackup;
    record.state = state;
    record.created_utc_ms = created;
    if (state != contracts::ServiceJobState::kQueued) {
        record.started_utc_ms = created + 1;
    }
    if (ports::is_terminal_job_state(state)) {
        record.completed_utc_ms = created + 2;
    }
    record.repository_connection_id = connection;
    record.source_id = "source-a";
    record.backup_type = contracts::BackupType::kFull;
    record.message_code = "job.queued";
    record.idempotency_key = "idem-" + id;
    return record;
}

bool test_command_store_persistence_and_uniqueness() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database)
        return false;

    auto unit = database.value()->begin_unit_of_work({});
    if (!unit)
        return false;
    ports::CommandRecord command{"idem-command", "fingerprint-a", "command-1", "resource-1", 100};
    bool passed = expect(unit.value()->commands().insert(command, {}).has_value(),
                         "command record inserts") &&
                  expect(unit.value()->commit({}).has_value(), "command record commits");
    auto loaded = database.value()->get_command("idem-command", {});
    passed &= expect(loaded && loaded.value() && loaded.value()->command_id == "command-1" &&
                         loaded.value()->request_fingerprint == "fingerprint-a" &&
                         loaded.value()->resource_id == "resource-1",
                     "command acknowledgement persists across the database facade");

    auto duplicate = database.value()->begin_unit_of_work({});
    if (!duplicate)
        return false;
    auto conflict = duplicate.value()->commands().insert(
        {"idem-command", "fingerprint-b", "command-2", "resource-2", 101}, {});
    passed &= expect(!conflict && conflict.error().code == base::ErrorCode::kConflict,
                     "idempotency key is unique");
    duplicate.value()->rollback();
    auto unchanged = database.value()->get_command("idem-command", {});
    passed &= expect(unchanged && unchanged.value() &&
                         unchanged.value()->request_fingerprint == "fingerprint-a",
                     "conflicting command does not replace durable acknowledgement");
    return passed;
}

bool test_schema_open_and_secret_ref_only() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    bool passed = expect(database.has_value(), "database opens and migrates");
    if (!database) {
        return false;
    }
    passed &= expect(database.value()->schema_version() == ports::kControlPlaneSchemaVersion,
                     "schema version is 1");
    auto unit = database.value()->begin_unit_of_work({});
    if (!expect(unit.has_value(), "begin unit of work")) {
        return false;
    }
    auto connection = make_connection("conn-1", "file:///repos/one", true, 1'700'000'000'000ULL);
    passed &= expect(unit.value()->repository_connections().upsert(connection, {}).has_value(),
                     "upsert repository connection");
    auto job =
        make_job("job-1", contracts::ServiceJobState::kQueued, 1'700'000'000'100ULL, "conn-1");
    passed &= expect(unit.value()->jobs().insert(job, {}).has_value(), "insert queued job");
    ports::ScheduleRecord schedule;
    schedule.schedule_id = "sched-1";
    schedule.display_name = "Nightly";
    schedule.enabled = true;
    schedule.source_id = "source-a";
    schedule.repository_connection_id = "conn-1";
    schedule.backup_type = contracts::BackupType::kFull;
    schedule.trigger.kind = contracts::ScheduleTriggerKind::kDaily;
    schedule.trigger.local_minute_of_day = 120;
    schedule.trigger.timezone_id = "UTC";
    schedule.next_run_utc_ms = 1'700'000'100'000ULL;
    schedule.created_utc_ms = 1'700'000'000'200ULL;
    schedule.updated_utc_ms = 1'700'000'000'200ULL;
    passed &= expect(unit.value()->schedules().upsert(schedule, {}).has_value(), "upsert schedule");
    ports::AuditEventRecord event;
    event.event_id = "evt-1";
    event.created_utc_ms = 1'700'000'000'300ULL;
    event.severity = contracts::AuditSeverity::kInformation;
    event.message_code = "repository.connection_added";
    event.message_arguments = {{"connection_id", "conn-1"}};
    event.correlation_id = "corr-1";
    passed &= expect(unit.value()->audit_events().append(event, {}).has_value(), "append audit");
    passed &= expect(unit.value()->commit({}).has_value(), "commit unit of work");

    auto loaded = database.value()->get_repository_connection("conn-1", {});
    passed &= expect(loaded && loaded.value() && loaded.value()->credential_ref &&
                         loaded.value()->credential_ref->value == "wincred://repo-conn-1",
                     "connection stores SecretRef only");
    passed &= expect(loaded.value()->locator == "file:///repos/one", "locator persisted");
    // Ensure no plaintext password column artifacts are required by scanning file for password.
    std::ifstream file(directory.database_path(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    passed &= expect(bytes.find("super-secret-password") == std::string::npos,
                     "database does not contain planted password");
    passed &= expect(bytes.find("wincred://repo-conn-1") != std::string::npos,
                     "database contains SecretRef locator");
    return passed;
}

bool test_job_state_machine_and_interrupt_convergence() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database) {
        return false;
    }
    auto unit = database.value()->begin_unit_of_work({});
    if (!unit) {
        return false;
    }
    auto connection = make_connection("conn-1", "file:///repos/one", true, 100);
    bool passed = expect(unit.value()->repository_connections().upsert(connection, {}).has_value(),
                         "seed connection");
    auto queued = make_job("job-run", contracts::ServiceJobState::kQueued, 200, "conn-1");
    passed &= expect(unit.value()->jobs().insert(queued, {}).has_value(), "insert job");
    auto illegal = unit.value()->jobs().transition(
        {"job-run", contracts::ServiceJobState::kQueued, contracts::ServiceJobState::kSucceeded,
         250, "job.succeeded", std::nullopt, std::nullopt, "job.succeeded"},
        {});
    passed &= expect(!illegal && illegal.error().code == aegra::base::ErrorCode::kInvalidArgument,
                     "illegal queued->succeeded rejected");
    auto running = unit.value()->jobs().transition(
        {"job-run", contracts::ServiceJobState::kQueued, contracts::ServiceJobState::kRunning, 260,
         "job.running", std::nullopt, std::nullopt, std::nullopt},
        {});
    passed &= expect(running && running.value().state == contracts::ServiceJobState::kRunning &&
                         running.value().started_utc_ms == 260,
                     "queued->running transition");
    auto cancelling = unit.value()->jobs().transition(
        {"job-run", contracts::ServiceJobState::kRunning, contracts::ServiceJobState::kCancelling,
         270, "job.cancelling", std::nullopt, std::nullopt, std::nullopt},
        {});
    passed &=
        expect(cancelling && cancelling.value().state == contracts::ServiceJobState::kCancelling,
               "running->cancelling transition");
    auto cas_miss = unit.value()->jobs().transition(
        {"job-run", contracts::ServiceJobState::kRunning, contracts::ServiceJobState::kFailed, 280,
         "job.failed", 9, 3, "job.failed"},
        {});
    passed &= expect(!cas_miss && cas_miss.error().code == aegra::base::ErrorCode::kConflict,
                     "CAS mismatch returns conflict");
    passed &= expect(unit.value()->commit({}).has_value(), "commit active job");

    // Simulate Service restart recovery on a fresh unit of work.
    auto recovery = database.value()->begin_unit_of_work({});
    if (!recovery) {
        return false;
    }
    auto interrupted = recovery.value()->jobs().mark_active_as_interrupted(500, {});
    passed &= expect(interrupted && interrupted.value() == 1, "one active job interrupted");
    passed &= expect(recovery.value()->commit({}).has_value(), "commit interrupt recovery");
    auto loaded = database.value()->get_job("job-run", {});
    passed &= expect(loaded && loaded.value() &&
                         loaded.value()->state == contracts::ServiceJobState::kInterrupted &&
                         loaded.value()->completed_utc_ms == 500,
                     "running/cancelling converges to interrupted");
    return passed;
}

bool test_foreign_keys_and_unique_constraints() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database) {
        return false;
    }
    auto unit = database.value()->begin_unit_of_work({});
    if (!unit) {
        return false;
    }
    bool passed = true;
    auto first = make_connection("conn-1", "file:///repos/one", true, 100);
    auto second = make_connection("conn-2", "file:///repos/one", false, 110);
    passed &= expect(unit.value()->repository_connections().upsert(first, {}).has_value(),
                     "first locator accepted");
    auto duplicate_locator = unit.value()->repository_connections().upsert(second, {});
    passed &= expect(!duplicate_locator &&
                         duplicate_locator.error().code == aegra::base::ErrorCode::kConflict,
                     "duplicate locator rejected");
    second.locator = "file:///repos/two";
    second.is_default = true;
    passed &= expect(unit.value()->repository_connections().upsert(second, {}).has_value(),
                     "second connection becomes sole default");
    auto listed =
        unit.value()->repository_connections().list({{50, std::nullopt}, std::nullopt}, {});
    passed &= expect(listed && listed.value().items.size() == 2, "two connections listed");
    if (listed) {
        const auto defaults = static_cast<int>(listed.value().items[0].is_default) +
                              static_cast<int>(listed.value().items[1].is_default);
        passed &= expect(defaults == 1, "exactly one default connection");
    }

    auto orphan_job =
        make_job("job-orphan", contracts::ServiceJobState::kQueued, 120, "missing-conn");
    auto orphan = unit.value()->jobs().insert(orphan_job, {});
    passed &= expect(!orphan && orphan.error().code == aegra::base::ErrorCode::kConflict,
                     "FK rejects orphan job");

    auto good_job = make_job("job-1", contracts::ServiceJobState::kQueued, 130, "conn-2");
    passed &= expect(unit.value()->jobs().insert(good_job, {}).has_value(), "job with valid FK");
    auto dup_idem = make_job("job-2", contracts::ServiceJobState::kQueued, 140, "conn-2");
    dup_idem.idempotency_key = good_job.idempotency_key;
    auto conflict = unit.value()->jobs().insert(dup_idem, {});
    passed &= expect(!conflict && conflict.error().code == aegra::base::ErrorCode::kConflict,
                     "unique idempotency key enforced");

    ports::ScheduleRecord schedule;
    schedule.schedule_id = "sched-1";
    schedule.display_name = "Daily";
    schedule.enabled = true;
    schedule.source_id = "source-a";
    schedule.repository_connection_id = "conn-2";
    schedule.backup_type = contracts::BackupType::kFull;
    schedule.trigger.kind = contracts::ScheduleTriggerKind::kWeekly;
    schedule.trigger.local_minute_of_day = 30;
    schedule.trigger.weekday_mask = 0x01;
    schedule.trigger.timezone_id = "UTC";
    schedule.created_utc_ms = 150;
    schedule.updated_utc_ms = 150;
    passed &= expect(unit.value()->schedules().upsert(schedule, {}).has_value(), "schedule FK ok");
    passed &= expect(unit.value()->repository_connections().remove("conn-2", {}).has_value(),
                     "remove connection cascades schedules");
    auto missing_schedule = unit.value()->schedules().get("sched-1", {});
    passed &= expect(missing_schedule && !missing_schedule.value(), "schedule removed by cascade");
    auto job_after = unit.value()->jobs().get("job-1", {});
    passed &= expect(job_after && job_after.value() && !job_after.value()->repository_connection_id,
                     "job connection set null on delete");
    passed &= expect(unit.value()->commit({}).has_value(), "commit constraint tests");
    return passed;
}

bool test_utc_and_transaction_rollback() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database) {
        return false;
    }
    auto unit = database.value()->begin_unit_of_work({});
    if (!unit) {
        return false;
    }
    auto connection = make_connection("conn-1", "file:///repos/one", true, 1'000);
    bool passed = expect(unit.value()->repository_connections().upsert(connection, {}).has_value(),
                         "upsert connection");
    auto bad_job = make_job("job-bad", contracts::ServiceJobState::kQueued, 900, "conn-1");
    // started without being non-queued is ok for queued; force invalid completed without terminal
    bad_job.completed_utc_ms = 950;
    auto invalid = unit.value()->jobs().insert(bad_job, {});
    passed &= expect(!invalid, "invalid timestamps rejected");
    unit.value()->rollback();
    auto listed =
        database.value()->list_repository_connections({{50, std::nullopt}, std::nullopt}, {});
    passed &=
        expect(listed && listed.value().items.empty(), "rollback discards uncommitted writes");
    return passed;
}

bool test_corrupt_database() {
    TemporaryDirectory directory;
    {
        std::ofstream file(directory.database_path(), std::ios::binary);
        file << "this is not a sqlite database";
    }
    auto database = open_db(directory.database_path(), sqlite_cp::SqliteOpenMode::kOpenExisting);
    return expect(!database && (database.error().code == aegra::base::ErrorCode::kCorruptData ||
                                database.error().code == aegra::base::ErrorCode::kIoFailure ||
                                database.error().code == aegra::base::ErrorCode::kInternal),
                  "corrupt database is rejected");
}

bool test_open_existing_missing() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path(), sqlite_cp::SqliteOpenMode::kOpenExisting);
    return expect(!database && database.error().code == aegra::base::ErrorCode::kNotFound,
                  "open existing rejects missing database");
}

bool test_concurrent_readers_with_single_writer() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database) {
        return false;
    }
    {
        auto unit = database.value()->begin_unit_of_work({});
        if (!unit) {
            return false;
        }
        auto connection = make_connection("conn-1", "file:///repos/one", true, 100);
        if (!unit.value()->repository_connections().upsert(connection, {}) ||
            !unit.value()->commit({})) {
            return false;
        }
    }

    std::atomic_bool readers_ok{true};
    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        readers.emplace_back([&database, &readers_ok] {
            for (int round = 0; round < 20; ++round) {
                auto page = database.value()->list_repository_connections(
                    {{10, std::nullopt}, std::nullopt}, {});
                if (!page || page.value().items.size() != 1) {
                    readers_ok = false;
                    return;
                }
            }
        });
    }

    bool writer_ok = true;
    for (int round = 0; round < 10; ++round) {
        std::unique_ptr<ports::IControlPlaneUnitOfWork> unit;
        for (int attempt = 0; attempt < 200; ++attempt) {
            auto began = database.value()->begin_unit_of_work({});
            if (began) {
                unit = std::move(began.value());
                break;
            }
            if (began.error().code != aegra::base::ErrorCode::kConflict) {
                writer_ok = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!unit) {
            writer_ok = false;
            break;
        }
        ports::AuditEventRecord event;
        event.event_id = "evt-" + std::to_string(round);
        event.created_utc_ms = static_cast<std::uint64_t>(200 + round);
        event.severity = contracts::AuditSeverity::kInformation;
        event.message_code = "job.progress";
        event.correlation_id = "corr-writer";
        if (!unit->audit_events().append(event, {}) || !unit->commit({})) {
            writer_ok = false;
            break;
        }
    }
    for (auto& reader : readers) {
        reader.join();
    }
    auto events = database.value()->list_audit_events(
        {{50, std::nullopt}, std::nullopt, std::nullopt, std::nullopt, std::nullopt}, {});
    return expect(writer_ok && readers_ok.load() && events && events.value().items.size() == 10,
                  "single writer and concurrent readers stay consistent");
}

bool test_second_unit_of_work_conflict() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database) {
        return false;
    }
    auto first = database.value()->begin_unit_of_work({});
    auto second = database.value()->begin_unit_of_work({});
    const bool passed = expect(first.has_value(), "first unit of work opens") &&
                        expect(!second && second.error().code == aegra::base::ErrorCode::kConflict,
                               "second concurrent write unit is rejected");
    if (first) {
        first.value()->rollback();
    }
    return passed;
}

bool test_unit_of_work_owns_database_lifetime() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database) {
        return false;
    }
    auto unit = database.value()->begin_unit_of_work({});
    if (!unit) {
        return false;
    }
    auto connection = make_connection("conn-owned", "file:///repos/owned", true, 100);
    if (!unit.value()->repository_connections().upsert(connection, {})) {
        return false;
    }

    database.value().reset();
    bool passed = expect(unit.value()->commit({}).has_value(),
                         "unit of work remains valid after database owner is released");
    unit.value().reset();

    auto reopened = open_db(directory.database_path(), sqlite_cp::SqliteOpenMode::kOpenExisting);
    if (!reopened) {
        return false;
    }
    auto loaded = reopened.value()->get_repository_connection("conn-owned", {});
    passed &= expect(loaded && loaded.value() && loaded.value()->is_default,
                     "state closes after the final unit-of-work owner is released");
    return passed;
}

bool test_finished_unit_of_work_rejects_store_access() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database) {
        return false;
    }
    auto unit = database.value()->begin_unit_of_work({});
    if (!unit) {
        return false;
    }
    auto committed = make_connection("conn-committed", "file:///repos/committed", true, 100);
    if (!unit.value()->repository_connections().upsert(committed, {}) ||
        !unit.value()->commit({})) {
        return false;
    }

    auto late = make_connection("conn-late", "file:///repos/late", false, 110);
    auto late_write = unit.value()->repository_connections().upsert(late, {});
    auto late_read = unit.value()->repository_connections().get("conn-committed", {});
    bool passed =
        expect(!late_write && late_write.error().code == aegra::base::ErrorCode::kConflict,
               "finished unit rejects transaction-external writes") &&
        expect(!late_read && late_read.error().code == aegra::base::ErrorCode::kConflict,
               "finished unit rejects transaction-external reads");
    auto listed =
        database.value()->list_repository_connections({{50, std::nullopt}, std::nullopt}, {});
    passed &= expect(listed && listed.value().items.size() == 1 &&
                         listed.value().items.front().connection_id == "conn-committed",
                     "rejected post-commit write is not persisted");
    return passed;
}

bool test_uncommitted_writes_not_visible_to_readers() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database) {
        return false;
    }
    auto unit = database.value()->begin_unit_of_work({});
    if (!unit) {
        return false;
    }
    auto connection = make_connection("conn-pending", "file:///repos/pending", true, 100);
    if (!unit.value()->repository_connections().upsert(connection, {})) {
        return false;
    }

    std::atomic_bool reader_started{false};
    std::atomic_bool reader_finished{false};
    std::atomic_size_t visible_count{999};
    std::thread reader([&] {
        reader_started = true;
        auto page =
            database.value()->list_repository_connections({{10, std::nullopt}, std::nullopt}, {});
        if (page) {
            visible_count = page.value().items.size();
        } else {
            visible_count = 999;
        }
        reader_finished = true;
    });

    while (!reader_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Reader must block on the shared write lock until this unit of work ends.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool still_blocked = !reader_finished.load();
    unit.value()->rollback();
    reader.join();
    return expect(still_blocked, "reader blocks while unit of work holds write lock") &&
           expect(visible_count.load() == 0,
                  "reader does not observe rolled-back uncommitted connection");
}

bool test_default_upsert_failure_is_atomic() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database) {
        return false;
    }
    auto unit = database.value()->begin_unit_of_work({});
    if (!unit) {
        return false;
    }
    auto first = make_connection("conn-1", "file:///repos/one", true, 100);
    auto second = make_connection("conn-2", "file:///repos/two", false, 110);
    bool passed = expect(unit.value()->repository_connections().upsert(first, {}).has_value(),
                         "seed default connection") &&
                  expect(unit.value()->repository_connections().upsert(second, {}).has_value(),
                         "seed second connection");
    auto conflicting = make_connection("conn-2", "file:///repos/one", true, 120);
    auto failed = unit.value()->repository_connections().upsert(conflicting, {});
    passed &= expect(!failed && failed.error().code == aegra::base::ErrorCode::kConflict,
                     "locator conflict fails default upsert");
    passed &= expect(unit.value()->commit({}).has_value(), "commit after failed store op");

    auto default_one = database.value()->get_repository_connection("conn-1", {});
    auto second_row = database.value()->get_repository_connection("conn-2", {});
    passed &= expect(default_one && default_one.value() && default_one.value()->is_default,
                     "original default survives failed default upsert");
    passed &= expect(second_row && second_row.value() && !second_row.value()->is_default &&
                         second_row.value()->locator == "file:///repos/two",
                     "conflicting connection row remains unchanged");
    return passed;
}

bool test_continuation_token_scope_and_filter() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database) {
        return false;
    }
    {
        auto unit = database.value()->begin_unit_of_work({});
        if (!unit) {
            return false;
        }
        for (int index = 0; index < 3; ++index) {
            auto connection = make_connection("conn-" + std::to_string(index),
                                              "file:///repos/" + std::to_string(index), index == 0,
                                              static_cast<std::uint64_t>(100 + index));
            if (!unit.value()->repository_connections().upsert(connection, {})) {
                return false;
            }
            auto job =
                make_job("job-" + std::to_string(index), contracts::ServiceJobState::kQueued,
                         static_cast<std::uint64_t>(200 + index), "conn-" + std::to_string(index));
            if (!unit.value()->jobs().insert(job, {})) {
                return false;
            }
        }
        if (!unit.value()->commit({})) {
            return false;
        }
    }

    auto connection_page =
        database.value()->list_repository_connections({{1, std::nullopt}, std::nullopt}, {});
    bool passed = expect(connection_page && connection_page.value().items.size() == 1 &&
                             connection_page.value().continuation_token.has_value(),
                         "connection page yields scoped token");
    if (!passed) {
        return false;
    }
    const auto job_with_connection_token = database.value()->list_jobs(
        {{1, connection_page.value().continuation_token}, std::nullopt, std::nullopt}, {});
    passed &= expect(!job_with_connection_token && job_with_connection_token.error().code ==
                                                       aegra::base::ErrorCode::kInvalidArgument,
                     "job list rejects repository continuation token");

    auto oversized_cursor = database.value()->list_repository_connections(
        {{1, std::string{"v1|rc|s%3d%2a|18446744073709551615|conn-0"}}, std::nullopt}, {});
    passed &= expect(!oversized_cursor &&
                         oversized_cursor.error().code == aegra::base::ErrorCode::kInvalidArgument,
                     "token rejects timestamp outside SQLite integer range");

    auto filtered = database.value()->list_repository_connections(
        {{1, std::nullopt}, contracts::RepositoryConnectionState::kAvailable}, {});
    passed &= expect(filtered && filtered.value().continuation_token.has_value(),
                     "filtered connection page yields token");
    if (filtered && filtered.value().continuation_token) {
        auto wrong_filter = database.value()->list_repository_connections(
            {{1, filtered.value().continuation_token}, std::nullopt}, {});
        passed &= expect(!wrong_filter &&
                             wrong_filter.error().code == aegra::base::ErrorCode::kInvalidArgument,
                         "token from filtered query is rejected without same filter");
        auto same_filter = database.value()->list_repository_connections(
            {{1, filtered.value().continuation_token},
             contracts::RepositoryConnectionState::kAvailable},
            {});
        passed &= expect(same_filter.has_value(), "token works with matching filter and kind");
    }
    return passed;
}

} // namespace

int main() {
    const bool passed =
        test_schema_open_and_secret_ref_only() && test_command_store_persistence_and_uniqueness() &&
        test_job_state_machine_and_interrupt_convergence() &&
        test_foreign_keys_and_unique_constraints() && test_utc_and_transaction_rollback() &&
        test_corrupt_database() && test_open_existing_missing() &&
        test_concurrent_readers_with_single_writer() && test_second_unit_of_work_conflict() &&
        test_unit_of_work_owns_database_lifetime() &&
        test_finished_unit_of_work_rejects_store_access() &&
        test_uncommitted_writes_not_visible_to_readers() &&
        test_default_upsert_failure_is_atomic() && test_continuation_token_scope_and_filter();
    if (!passed) {
        std::cerr << "sqlite control plane tests failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "sqlite control plane tests passed\n";
    return EXIT_SUCCESS;
}
