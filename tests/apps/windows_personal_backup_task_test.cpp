#include "aegra/apps/worker/windows_personal_backup_task.h"

#include "windows_personal_backup_task_backend.h"

#include "aegra/apps/worker/windows_personal_backup.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace app = aegra::apps::worker;
namespace contracts = aegra::contracts;
namespace detail = aegra::apps::worker::detail;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

struct TestState final {
    std::size_t resolve_count{0};
    std::size_t random_count{0};
    std::size_t backend_count{0};
    bool secret_destroyed{false};
    bool fail_resolve{false};
    bool fail_random{false};
    bool cleanup_warning{false};
    bool progress_capture_failed{false};
    std::optional<aegra::base::Error> backend_error;
    std::string resolved_ref;
    std::string received_password;
    std::string received_job_id;
    std::string received_trace_id;
    std::string received_created_utc;
    std::filesystem::path received_source;
    std::filesystem::path received_destination;
    std::array<std::byte, 16> file_uuid{};
    std::array<std::byte, 16> backup_set_uuid{};
    aegra::ports::IProgressSink* received_progress{nullptr};
    std::vector<contracts::TaskProgress> progress;
};

class TestSecret final : public aegra::ports::IResolvedSecret {
  public:
    TestSecret(std::shared_ptr<TestState> state, std::string value)
        : state_(std::move(state)), value_(std::move(value)) {}
    ~TestSecret() override {
        std::ranges::fill(value_, '\0');
        state_->secret_destroyed = true;
    }

    TestSecret(const TestSecret&) = delete;
    TestSecret& operator=(const TestSecret&) = delete;
    TestSecret(TestSecret&&) = delete;
    TestSecret& operator=(TestSecret&&) = delete;

    [[nodiscard]] std::string_view view() const noexcept override { return value_; }

  private:
    std::shared_ptr<TestState> state_;
    std::string value_;
};

class TestCredentialResolver final : public aegra::ports::ICredentialResolver {
  public:
    explicit TestCredentialResolver(std::shared_ptr<TestState> state) : state_(std::move(state)) {}

    [[nodiscard]] aegra::base::Result<std::unique_ptr<aegra::ports::IResolvedSecret>>
    resolve(const contracts::SecretRef& secret_ref,
            const aegra::base::CancellationToken& cancellation) override {
        ++state_->resolve_count;
        state_->resolved_ref = secret_ref.value;
        if (cancellation.stop_requested()) {
            return aegra::base::Result<std::unique_ptr<aegra::ports::IResolvedSecret>>::failure(
                {aegra::base::ErrorCode::kCancelled, "test credential resolution cancelled"});
        }
        if (state_->fail_resolve) {
            return aegra::base::Result<std::unique_ptr<aegra::ports::IResolvedSecret>>::failure(
                {aegra::base::ErrorCode::kUnauthorized, "secret backend detail"});
        }
        std::unique_ptr<aegra::ports::IResolvedSecret> secret =
            std::make_unique<TestSecret>(state_, "test-password");
        return aegra::base::Result<std::unique_ptr<aegra::ports::IResolvedSecret>>::success(
            std::move(secret));
    }

  private:
    std::shared_ptr<TestState> state_;
};

class TestRandomSource final : public aegra::ports::IRandomSource {
  public:
    explicit TestRandomSource(std::shared_ptr<TestState> state) : state_(std::move(state)) {}

    [[nodiscard]] aegra::base::Result<void>
    fill(const std::span<std::byte> destination,
         const aegra::base::CancellationToken& cancellation) override {
        if (cancellation.stop_requested()) {
            return aegra::base::Result<void>::failure(
                {aegra::base::ErrorCode::kCancelled, "test random generation cancelled"});
        }
        if (state_->fail_random) {
            return aegra::base::Result<void>::failure(
                {aegra::base::ErrorCode::kIoFailure, "random backend detail"});
        }
        ++state_->random_count;
        std::ranges::fill(destination, static_cast<std::byte>(state_->random_count));
        return aegra::base::Result<void>::success();
    }

  private:
    std::shared_ptr<TestState> state_;
};

class TestClock final : public aegra::ports::IClock {
  public:
    explicit TestClock(const std::int64_t now_utc_ms) : now_utc_ms_(now_utc_ms) {}

    [[nodiscard]] std::int64_t now_utc_ms() const noexcept override { return now_utc_ms_; }

  private:
    std::int64_t now_utc_ms_;
};

class TestProgressSink final : public aegra::ports::IProgressSink {
  public:
    explicit TestProgressSink(std::shared_ptr<TestState> state) : state_(std::move(state)) {}

    void publish(const contracts::TaskProgress& progress) noexcept override {
        try {
            state_->progress.push_back(progress);
        } catch (...) {
            state_->progress_capture_failed = true;
        }
    }

  private:
    std::shared_ptr<TestState> state_;
};

class TestBackend final : public detail::IWindowsPersonalBackupTaskBackend {
  public:
    explicit TestBackend(std::shared_ptr<TestState> state) : state_(std::move(state)) {}

    [[nodiscard]] aegra::base::Result<app::WindowsPersonalVolumeBackupResult>
    run(const app::WindowsPersonalVolumeBackupRequest& request,
        const aegra::base::CancellationToken&, aegra::ports::IProgressSink* progress) override {
        ++state_->backend_count;
        state_->received_password = request.password;
        state_->received_job_id = request.job_id;
        state_->received_trace_id = request.trace_id;
        state_->received_created_utc = request.created_utc;
        state_->received_source = request.volume_guid_path;
        state_->received_destination = request.destination;
        state_->file_uuid = request.file_uuid;
        state_->backup_set_uuid = request.backup_set_uuid;
        state_->received_progress = progress;
        if (state_->backend_error) {
            return aegra::base::Result<app::WindowsPersonalVolumeBackupResult>::failure(
                *state_->backend_error);
        }
        app::WindowsPersonalVolumeBackupResult result;
        result.backup = {100, 80, 4, 32};
        if (state_->cleanup_warning) {
            result.snapshot_cleanup_error =
                aegra::base::Error{aegra::base::ErrorCode::kIoFailure, "snapshot backend detail"};
        }
        return aegra::base::Result<app::WindowsPersonalVolumeBackupResult>::success(
            std::move(result));
    }

  private:
    std::shared_ptr<TestState> state_;
};

contracts::JobRequest valid_job() {
    contracts::JobRequest job;
    job.job_id = "job-personal-1";
    job.tenant_id = "tenant-1";
    job.operation = contracts::JobOperation::kBackup;
    job.source_refs = {R"(\\?\Volume{01234567-89ab-cdef-0123-456789abcdef}\)"};
    job.target_ref = R"(D:\Backups\personal.bkf)";
    job.credential_refs = {contracts::SecretRef{"secret://personal/password"}};
    job.trace_id = "trace-personal-1";
    return job;
}

app::WindowsPersonalBackupTaskOptions valid_options() {
    app::WindowsPersonalBackupTaskOptions options;
    options.block_size_bytes = 4096;
    options.chunk_size_bytes = 1024 * 1024;
    options.memory_budget_bytes = 4ULL * 1024ULL * 1024ULL;
    options.application_version = "0.1.0";
    options.hostname = "test-host";
    return options;
}

struct TestFixture final {
    std::shared_ptr<TestState> state{std::make_shared<TestState>()};
    TestCredentialResolver credentials{state};
    TestRandomSource random{state};
    TestClock clock{1234};
    TestProgressSink progress{state};
    TestBackend backend{state};

    [[nodiscard]] app::WindowsPersonalBackupTaskContext context() {
        return {credentials, random, clock, &progress};
    }
};

bool test_successful_mapping() {
    TestFixture fixture;
    auto context = fixture.context();
    auto result = detail::execute_windows_personal_backup_task_with_backend(
        valid_job(), valid_options(), context, {}, fixture.backend);
    bool passed = expect(result && result.value().outcome == contracts::TaskOutcome::kSucceeded,
                         "successful backend produces successful task result");
    passed &= expect(result && result.value().logical_bytes == 100 &&
                         result.value().stored_bytes == 80 && result.value().chunk_count == 4,
                     "task result maps backup metrics");
    passed &= expect(result && contracts::validate_task_result(result.value()).has_value(),
                     "task executor emits a valid result contract");
    passed &= expect(fixture.state->resolve_count == 1 && fixture.state->random_count == 2 &&
                         fixture.state->backend_count == 1 && fixture.state->secret_destroyed,
                     "executor resolves and releases one credential around one backend call");
    passed &= expect(fixture.state->resolved_ref == "secret://personal/password" &&
                         fixture.state->received_password == "test-password",
                     "executor resolves the requested secret without placing it in the job");
    passed &= expect(fixture.state->received_job_id == "job-personal-1" &&
                         fixture.state->received_trace_id == "trace-personal-1" &&
                         fixture.state->received_created_utc == "1970-01-01T00:00:01.234Z",
                     "executor maps correlation and deterministic creation time");
    passed &= expect(fixture.state->file_uuid != fixture.state->backup_set_uuid &&
                         (fixture.state->file_uuid[6] & std::byte{0xF0}) == std::byte{0x40} &&
                         (fixture.state->file_uuid[8] & std::byte{0xC0}) == std::byte{0x80},
                     "executor creates distinct RFC 4122 version 4 identifiers");
    passed &= expect(fixture.state->received_progress == &fixture.progress &&
                         fixture.state->progress.size() == 1 &&
                         fixture.state->progress.front().trace_id == "trace-personal-1" &&
                         fixture.state->progress.front().message_code == "backup.preparing",
                     "executor publishes correlated preparation progress");
    return passed;
}

bool test_warning_and_failure_mapping() {
    TestFixture warning_fixture;
    warning_fixture.state->cleanup_warning = true;
    auto warning_context = warning_fixture.context();
    auto warning = detail::execute_windows_personal_backup_task_with_backend(
        valid_job(), valid_options(), warning_context, {}, warning_fixture.backend);
    bool passed =
        expect(warning && warning.value().outcome == contracts::TaskOutcome::kSucceededWithWarning,
               "snapshot cleanup failure preserves committed task success");
    passed &= expect(warning && warning.value().warning_codes.size() == 1 &&
                         warning.value().warning_codes.front() == "backup.snapshot_cleanup_failed",
                     "snapshot cleanup failure uses a stable warning code");

    TestFixture failure_fixture;
    failure_fixture.state->backend_error = aegra::base::Error{
        aegra::base::ErrorCode::kIoFailure,
        "backend detail includes test-password and D:\\customer",
    };
    auto failure_context = failure_fixture.context();
    auto failure = detail::execute_windows_personal_backup_task_with_backend(
        valid_job(), valid_options(), failure_context, {}, failure_fixture.backend);
    passed &= expect(failure && failure.value().outcome == contracts::TaskOutcome::kFailed &&
                         failure.value().message_code == "backup.failed",
                     "backend detail is replaced with a stable failure code");
    passed &=
        expect(failure && failure.value().message_code.find("test-password") == std::string::npos,
               "task result does not expose backend secret text");
    passed &= expect(failure_fixture.state->secret_destroyed,
                     "failure path releases the resolved secret");
    return passed;
}

bool test_rejection_cancellation_and_dependency_failures() {
    TestFixture invalid_fixture;
    auto invalid_job = valid_job();
    invalid_job.source_refs.push_back("second-source");
    auto invalid_context = invalid_fixture.context();
    auto invalid = detail::execute_windows_personal_backup_task_with_backend(
        invalid_job, valid_options(), invalid_context, {}, invalid_fixture.backend);
    bool passed =
        expect(!invalid && invalid.error().code == aegra::base::ErrorCode::kInvalidArgument,
               "operation-specific contract violation rejects the task");
    passed &=
        expect(invalid_fixture.state->progress.empty() && invalid_fixture.state->resolve_count == 0,
               "rejected task acquires no runtime dependencies");

    TestFixture cancelled_fixture;
    aegra::base::CancellationSource cancellation;
    cancellation.request_stop();
    auto cancelled_context = cancelled_fixture.context();
    auto cancelled = detail::execute_windows_personal_backup_task_with_backend(
        valid_job(), valid_options(), cancelled_context, cancellation.get_token(),
        cancelled_fixture.backend);
    passed &= expect(cancelled && cancelled.value().outcome == contracts::TaskOutcome::kCancelled,
                     "pre-cancelled task returns cancelled result");
    passed &= expect(cancelled_fixture.state->resolve_count == 0 &&
                         cancelled_fixture.state->backend_count == 0,
                     "pre-cancelled task resolves no secret and starts no backup");

    TestFixture expired_fixture;
    auto expired_job = valid_job();
    expired_job.deadline_utc_ms = 1000;
    auto expired_context = expired_fixture.context();
    auto expired = detail::execute_windows_personal_backup_task_with_backend(
        expired_job, valid_options(), expired_context, {}, expired_fixture.backend);
    passed &= expect(expired && expired.value().outcome == contracts::TaskOutcome::kCancelled &&
                         expired_fixture.state->resolve_count == 0,
                     "expired deadline cancels before credential resolution");

    TestFixture credential_fixture;
    credential_fixture.state->fail_resolve = true;
    auto credential_context = credential_fixture.context();
    auto credential = detail::execute_windows_personal_backup_task_with_backend(
        valid_job(), valid_options(), credential_context, {}, credential_fixture.backend);
    passed &=
        expect(credential && credential.value().message_code == "backup.credential_unavailable" &&
                   credential_fixture.state->backend_count == 0,
               "credential failure is sanitized and prevents backup start");
    return passed;
}

int run_tests() {
    const bool passed = test_successful_mapping() && test_warning_and_failure_mapping() &&
                        test_rejection_cancellation_and_dependency_failures();
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
