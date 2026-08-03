#include "aegra/adapters/sqlite/sqlite_control_plane.h"
#include "aegra/adapters/windows_process/windows_process_launcher.h"
#include "aegra/apps/service/worker_supervisor.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/random.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <thread>

namespace {

namespace service = aegra::apps::service;
namespace sqlite = aegra::adapters::sqlite;
namespace windows_process = aegra::adapters::windows_process;

bool expect(const bool condition, const char* message) {
    if (condition)
        return true;
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

class TestClock final : public aegra::ports::IClock {
  public:
    [[nodiscard]] std::int64_t now_utc_ms() const noexcept override { return 1'800'000'000'000; }
};

class TestRandom final : public aegra::ports::IRandomSource {
  public:
    [[nodiscard]] aegra::base::Result<void>
    fill(const std::span<std::byte> destination,
         const aegra::base::CancellationToken& cancellation) override {
        if (cancellation.stop_requested()) {
            return aegra::base::Result<void>::failure(
                {aegra::base::ErrorCode::kCancelled, "test random cancelled"});
        }
        for (auto& value : destination) {
            value = static_cast<std::byte>(next_++);
        }
        return aegra::base::Result<void>::success();
    }

  private:
    std::uint8_t next_{1};
};

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("aegra_supervisor_" + std::to_string(GetCurrentProcessId()));
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const auto& value = path.native();
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const auto input_size = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                             input_size, nullptr, 0, nullptr, nullptr);
    if (required == 0)
        return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
                               result.data(), required, nullptr, nullptr) == 0
               ? std::string{}
               : result;
}

bool seed_repository(aegra::ports::IControlPlaneDatabase& database) {
    auto unit = database.begin_unit_of_work({});
    if (!unit)
        return false;
    aegra::ports::RepositoryConnectionRecord connection;
    connection.connection_id = "connection-1";
    connection.display_name = "Supervisor test";
    connection.locator = "C:/aegra-supervisor-test";
    connection.state = aegra::contracts::RepositoryConnectionState::kAvailable;
    connection.is_default = true;
    connection.created_utc_ms = 1;
    connection.updated_utc_ms = 1;
    auto stored = unit.value()->repository_connections().upsert(connection, {});
    return stored && unit.value()->commit({});
}

service::WorkerJobRequest make_request(const TemporaryDirectory& directory) {
    aegra::contracts::JobRequest worker;
    worker.job_id = "job-supervisor-process";
    worker.tenant_id = "personal";
    worker.operation = aegra::contracts::JobOperation::kBackup;
    worker.source_refs = {R"(\\?\Volume{00000000-0000-0000-0000-000000000000}\)"};
    worker.target_ref = path_to_utf8(directory.path() / "result.bkf");
    worker.credential_refs = {aegra::contracts::SecretRef{"wincred://aegra-missing-test"}};
    worker.backup = aegra::contracts::BackupOptions{};
    worker.trace_id = "trace-supervisor-process";

    service::WorkerJobRequest request;
    request.worker_request = std::move(worker);
    request.source_id = "source-1";
    request.repository_connection_id = "connection-1";
    request.idempotency_key = "idempotency-supervisor-process";
    request.deadline = std::chrono::seconds(15);
    return request;
}

bool run_test(const std::filesystem::path& worker_path) {
    TemporaryDirectory directory;
    auto database = sqlite::SqliteControlPlaneDatabase::open(
        {directory.path() / "control-plane.db", sqlite::SqliteOpenMode::kCreateIfMissing});
    if (!expect(database.has_value(), "control plane opens") ||
        !expect(seed_repository(*database.value()), "repository fixture is stored")) {
        return false;
    }

    TestClock clock;
    TestRandom random;
    windows_process::WindowsProcessLauncher launcher;
    service::WorkerSupervisorConfig config;
    config.worker_executable_path = path_to_utf8(worker_path);
    config.default_job_deadline = std::chrono::seconds(15);
    config.stop_drain_timeout = std::chrono::seconds(2);
    service::WorkerSupervisor supervisor(std::move(config), launcher, *database.value(), clock,
                                         random, {}, {});
    auto submitted = supervisor.submit(make_request(directory), {});
    if (!expect(submitted.has_value(), "Supervisor accepts a real Worker job"))
        return false;

    std::optional<aegra::ports::JobRecord> record;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        auto loaded = database.value()->get_job("job-supervisor-process", {});
        if (loaded && loaded.value() &&
            aegra::ports::is_terminal_job_state(loaded.value()->state)) {
            record = std::move(*loaded.value());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    bool passed = expect(record.has_value(), "real Worker job reaches a terminal state");
    passed &= expect(record && record->state == aegra::contracts::ServiceJobState::kFailed,
                     "missing credential is persisted as a failed task result");
    passed &= expect(record && record->result_message_code == "backup.credential_unavailable",
                     "authoritative Worker result is persisted");
    supervisor.shutdown({});
    passed &= expect(supervisor.active_count() == 0, "completed Worker session is reaped");
    return passed;
}

} // namespace

int wmain(const int argument_count, wchar_t* arguments[]) noexcept {
    try {
        if (argument_count != 2) {
            std::fputs("[FAIL] expected Worker executable path\n", stderr);
            return EXIT_FAILURE;
        }
        return run_test(arguments[1]) ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
