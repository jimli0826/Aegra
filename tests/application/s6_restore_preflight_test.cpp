#include "aegra/adapters/sqlite/sqlite_control_plane.h"
#include "aegra/application/restore_preflight_service.h"
#include "aegra/application/source_inventory_query.h"
#include "aegra/base/error.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/random.h"
#include "aegra/ports/source_inventory.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace application = aegra::application;
namespace base = aegra::base;
namespace contracts = aegra::contracts;
namespace ports = aegra::ports;
namespace sqlite_cp = aegra::adapters::sqlite;

bool expect(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "[FAIL] %s\n", message);
    }
    return condition;
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{0};
        path_ = std::filesystem::temp_directory_path() /
                ("aegra-s6-" +
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

class FakeClock final : public ports::IClock {
  public:
    explicit FakeClock(const std::int64_t now) noexcept : now_(now) {}
    [[nodiscard]] std::int64_t now_utc_ms() const noexcept override { return now_; }

  private:
    std::int64_t now_;
};

class FakeRandom final : public ports::IRandomSource {
  public:
    [[nodiscard]] base::Result<void> fill(const std::span<std::byte> destination,
                                          const base::CancellationToken& cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure({base::ErrorCode::kCancelled, "cancelled"});
        }
        for (std::size_t index = 0; index < destination.size(); ++index) {
            destination[index] = static_cast<std::byte>(index + 1U);
        }
        return base::Result<void>::success();
    }
};

class FakeInventory final : public ports::ISourceInventory {
  public:
    std::vector<ports::SourceInventoryRecord> records;

    [[nodiscard]] base::Result<std::vector<ports::SourceInventoryRecord>>
    list_sources(const base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<std::vector<ports::SourceInventoryRecord>>::failure(
                {base::ErrorCode::kCancelled, "cancelled"});
        }
        return base::Result<std::vector<ports::SourceInventoryRecord>>::success(records);
    }
};

class FakeChainInspector final : public application::IRestoreChainInspector {
  public:
    application::RestoreChainSnapshot snapshot{"repository-uuid-1", "sha256:0123456789abcdef",
                                               4'096, 3};
    std::optional<base::Error> failure;
    std::size_t calls{0};

    [[nodiscard]] base::Result<application::RestoreChainSnapshot>
    inspect(const ports::RepositoryConnectionRecord&, const std::string_view,
            const base::CancellationToken cancellation) override {
        ++calls;
        if (cancellation.stop_requested()) {
            return base::Result<application::RestoreChainSnapshot>::failure(
                {base::ErrorCode::kCancelled, "cancelled"});
        }
        if (failure) {
            return base::Result<application::RestoreChainSnapshot>::failure(*failure);
        }
        return base::Result<application::RestoreChainSnapshot>::success(snapshot);
    }
};

[[nodiscard]] base::Result<std::unique_ptr<sqlite_cp::SqliteControlPlaneDatabase>>
open_database(const std::filesystem::path& path) {
    return sqlite_cp::SqliteControlPlaneDatabase::open(
        {path, sqlite_cp::SqliteOpenMode::kCreateIfMissing});
}

bool seed_connection(ports::IControlPlaneDatabase& database) {
    auto unit = database.begin_unit_of_work({});
    if (!unit) {
        return false;
    }
    ports::RepositoryConnectionRecord connection;
    connection.connection_id = "repository-1";
    connection.display_name = "Repository";
    connection.locator = "file:///trusted/repository";
    connection.state = contracts::RepositoryConnectionState::kAvailable;
    connection.is_default = true;
    connection.created_utc_ms = 1'000;
    connection.updated_utc_ms = 1'000;
    return unit.value()->repository_connections().upsert(connection, {}) &&
           unit.value()->commit({});
}

[[nodiscard]] ports::SourceInventoryRecord target_record() {
    return {.source_id = "volume-target-1",
            .stable_key = "trusted-volume-identity",
            .display_name = "Restore target",
            .kind = contracts::SourceKind::kVolume,
            .availability = contracts::SourceAvailability::kAvailable,
            .capacity_bytes = 8'192,
            .is_system = false,
            .is_read_only = false};
}

[[nodiscard]] contracts::RestorePreflightRequest request() {
    return {"repository-1", "recovery-point-1", "volume-target-1"};
}

bool test_prepare_persists_trusted_snapshot() {
    TemporaryDirectory directory;
    auto database = open_database(directory.database_path());
    if (!database || !seed_connection(*database.value())) {
        return false;
    }
    FakeInventory inventory;
    inventory.records = {target_record()};
    application::SourceInventoryQuery source_query(inventory);
    FakeChainInspector chain;
    FakeClock clock(50'000);
    FakeRandom random;
    application::RestorePreflightService service(*database.value(), chain, source_query, clock,
                                                 random);

    auto prepared = service.prepare(request(), {});
    bool passed = expect(prepared && prepared.value().restore_eligible,
                         "eligible restore preflight succeeds");
    if (!prepared) {
        return false;
    }
    passed &=
        expect(prepared.value().logical_size_bytes == 4'096 &&
                   prepared.value().target_capacity_bytes == 8'192 &&
                   prepared.value().chain_depth == 3 && prepared.value().expires_utc_ms == 350'000,
               "preflight exposes trusted size, depth, capacity, and bounded TTL");
    auto durable = database.value()->get_restore_preflight(prepared.value().preflight_token, {});
    passed &= expect(durable && durable.value() &&
                         durable.value()->repository_uuid == chain.snapshot.repository_uuid &&
                         durable.value()->chain_fingerprint == chain.snapshot.chain_fingerprint &&
                         durable.value()->target_source_id == request().target_source_id,
                     "preflight snapshot is durable without Desktop-provided paths");

    auto duplicate = service.prepare(request(), {});
    passed &= expect(!duplicate && duplicate.error().code == base::ErrorCode::kConflict,
                     "opaque token collision cannot replace a preflight");
    return passed;
}

bool test_repository_chain_and_cancellation_failures() {
    TemporaryDirectory directory;
    auto database = open_database(directory.database_path());
    if (!database) {
        return false;
    }
    FakeInventory inventory;
    inventory.records = {target_record()};
    application::SourceInventoryQuery source_query(inventory);
    FakeChainInspector chain;
    FakeClock clock(50'000);
    FakeRandom random;
    application::RestorePreflightService service(*database.value(), chain, source_query, clock,
                                                 random);

    auto missing = service.prepare(request(), {});
    bool passed =
        expect(!missing && missing.error().code == base::ErrorCode::kNotFound && chain.calls == 0,
               "missing repository fails before chain inspection");
    if (!seed_connection(*database.value())) {
        return false;
    }
    for (const auto code : {base::ErrorCode::kNotFound, base::ErrorCode::kUnauthorized,
                            base::ErrorCode::kCorruptData, base::ErrorCode::kIoFailure}) {
        chain.failure = base::Error{code, "stable fake failure"};
        auto failed = service.prepare(request(), {});
        passed &= expect(!failed && failed.error().code == code,
                         "chain inspection failure remains stable");
    }
    chain.failure.reset();
    base::CancellationSource cancellation;
    cancellation.request_stop();
    auto cancelled = service.prepare(request(), cancellation.get_token());
    passed &= expect(!cancelled && cancelled.error().code == base::ErrorCode::kCancelled,
                     "preflight observes cancellation before side effects");
    return passed;
}

bool test_target_and_snapshot_safety() {
    TemporaryDirectory directory;
    auto database = open_database(directory.database_path());
    if (!database || !seed_connection(*database.value())) {
        return false;
    }
    FakeInventory inventory;
    inventory.records = {target_record()};
    application::SourceInventoryQuery source_query(inventory);
    FakeChainInspector chain;
    FakeClock clock(50'000);
    FakeRandom random;
    application::RestorePreflightService service(*database.value(), chain, source_query, clock,
                                                 random);

    inventory.records.front().is_system = true;
    auto system = service.prepare(request(), {});
    bool passed = expect(!system && system.error().code == base::ErrorCode::kConflict,
                         "system target is rejected");
    inventory.records.front() = target_record();
    inventory.records.front().capacity_bytes = 4'095;
    auto undersized = service.prepare(request(), {});
    passed &= expect(!undersized && undersized.error().code == base::ErrorCode::kConflict,
                     "undersized target is rejected");
    inventory.records.front() = target_record();
    inventory.records.front().kind = static_cast<contracts::SourceKind>(2);
    auto disk = service.prepare(request(), {});
    passed &= expect(!disk && disk.error().code == base::ErrorCode::kConflict,
                     "physical disk target is rejected");
    inventory.records.front() = target_record();
    chain.snapshot.chain_fingerprint.clear();
    auto invalid_snapshot = service.prepare(request(), {});
    passed &= expect(!invalid_snapshot &&
                         invalid_snapshot.error().code == base::ErrorCode::kInvalidArgument,
                     "malformed trusted chain snapshot is rejected");
    return passed;
}

} // namespace

int main() {
    const bool passed = test_prepare_persists_trusted_snapshot() &&
                        test_repository_chain_and_cancellation_failures() &&
                        test_target_and_snapshot_safety();
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
