#include "aegra/adapters/sqlite/sqlite_control_plane.h"
#include "aegra/base/error.h"
#include "aegra/ports/control_plane.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace {

namespace base = aegra::base;
namespace contracts = aegra::contracts;
namespace ports = aegra::ports;
namespace sqlite_cp = aegra::adapters::sqlite;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{0};
        path_ = std::filesystem::temp_directory_path() /
                ("aegra-sqlite-restore-" +
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

ports::RepositoryConnectionRecord make_connection() {
    ports::RepositoryConnectionRecord record;
    record.connection_id = "conn-1";
    record.display_name = "Repository";
    record.locator = "file:///repos/one";
    record.state = contracts::RepositoryConnectionState::kAvailable;
    record.is_default = true;
    record.created_utc_ms = 2'000;
    record.updated_utc_ms = 2'000;
    return record;
}

ports::RestorePreflightRecord make_preflight(const std::string& token) {
    return {.preflight_token = token,
            .repository_connection_id = "conn-1",
            .repository_uuid = "repository-uuid-1",
            .recovery_point_id = "recovery-1",
            .target_source_id = "source-target-1",
            .chain_fingerprint = "sha256:0123456789abcdef",
            .logical_size_bytes = 1'024,
            .target_capacity_bytes = 2'048,
            .chain_depth = 2,
            .created_utc_ms = 1'000,
            .expires_utc_ms = 301'000};
}

ports::JobRecord make_restore_job(const std::string& id, const std::string& token) {
    ports::JobRecord record;
    record.job_id = id;
    record.trace_id = "trace-" + id;
    record.operation = contracts::JobOperation::kRestore;
    record.state = contracts::ServiceJobState::kQueued;
    record.created_utc_ms = 2'100;
    record.source_id = "recovery-1";
    record.repository_connection_id = "conn-1";
    record.target_source_id = "source-target-1";
    record.preflight_token = token;
    record.message_code = "job.queued";
    record.idempotency_key = "idem-" + id;
    return record;
}

bool test_persistence_validation_and_lifetime() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database) {
        return false;
    }
    auto unit = database.value()->begin_unit_of_work({});
    if (!unit) {
        return false;
    }
    auto invalid = make_preflight("preflight-invalid");
    invalid.target_capacity_bytes = invalid.logical_size_bytes - 1;
    bool passed = expect(!unit.value()->restore_preflights().insert(invalid, {}),
                         "undersized preflight is rejected");
    const auto preflight = make_preflight("preflight-1");
    passed &= expect(unit.value()->restore_preflights().insert(preflight, {}).has_value(),
                     "preflight inserts");
    auto duplicate = unit.value()->restore_preflights().insert(preflight, {});
    passed &= expect(!duplicate && duplicate.error().code == base::ErrorCode::kConflict,
                     "preflight token is unique");
    passed &= expect(unit.value()->commit({}).has_value(), "preflight commits");
    auto inactive = unit.value()->restore_preflights().get(preflight.preflight_token, {});
    passed &= expect(!inactive && inactive.error().code == base::ErrorCode::kConflict,
                     "finished unit rejects store access");

    database.value().reset();
    database = open_db(directory.database_path(), sqlite_cp::SqliteOpenMode::kOpenExisting);
    if (!database) {
        return false;
    }
    auto loaded = database.value()->get_restore_preflight(preflight.preflight_token, {});
    passed &= expect(loaded && loaded.value() &&
                         loaded.value()->repository_uuid == preflight.repository_uuid &&
                         loaded.value()->chain_fingerprint == preflight.chain_fingerprint,
                     "preflight survives reopen");
    base::CancellationSource cancellation;
    cancellation.request_stop();
    auto cancelled = database.value()->get_restore_preflight(preflight.preflight_token,
                                                             cancellation.get_token());
    passed &= expect(!cancelled && cancelled.error().code == base::ErrorCode::kCancelled,
                     "preflight read observes cancellation");
    return passed;
}

bool test_job_token_ownership_and_rollback() {
    TemporaryDirectory directory;
    auto database = open_db(directory.database_path());
    if (!database) {
        return false;
    }
    auto unit = database.value()->begin_unit_of_work({});
    if (!unit) {
        return false;
    }
    bool passed =
        expect(unit.value()->repository_connections().upsert(make_connection(), {}).has_value(),
               "seed repository connection");
    const auto first = make_restore_job("restore-job-1", "preflight-1");
    passed &=
        expect(unit.value()->jobs().insert(first, {}).has_value(), "first job occupies token");
    auto occupied =
        unit.value()->jobs().insert(make_restore_job("restore-job-2", "preflight-1"), {});
    passed &= expect(!occupied && occupied.error().code == base::ErrorCode::kConflict,
                     "one token creates at most one job");
    passed &= expect(unit.value()->commit({}).has_value(), "job token ownership commits");
    auto by_token = database.value()->get_job_by_preflight_token("preflight-1", {});
    passed &= expect(by_token && by_token.value() && by_token.value()->job_id == first.job_id,
                     "job is found by preflight token");

    auto rollback = database.value()->begin_unit_of_work({});
    if (!rollback) {
        return false;
    }
    const auto rolled_back = make_preflight("preflight-rollback");
    passed &= expect(rollback.value()->restore_preflights().insert(rolled_back, {}).has_value(),
                     "preflight inserts before rollback");
    rollback.value()->rollback();
    auto missing = database.value()->get_restore_preflight(rolled_back.preflight_token, {});
    passed &= expect(missing && !missing.value(), "rollback discards preflight");
    return passed;
}

} // namespace

int main() {
    return test_persistence_validation_and_lifetime() && test_job_token_ownership_and_rollback()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
