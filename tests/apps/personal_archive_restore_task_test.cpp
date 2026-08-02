#include "aegra/apps/worker/personal_archive_restore_task.h"

#include "personal_archive_restore_task_backend.h"

#include "aegra/base/error.h"
#include "aegra/contracts/job.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/credential.h"
#include "aegra/ports/random.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

namespace app = aegra::apps::worker;
namespace base = aegra::base;
namespace contracts = aegra::contracts;
namespace detail = aegra::apps::worker::detail;

class Secret final : public aegra::ports::IResolvedSecret {
  public:
    explicit Secret(bool& destroyed) : destroyed_(destroyed) {}
    ~Secret() override { destroyed_ = true; }
    std::string_view view() const noexcept override { return "restore-password"; }

  private:
    bool& destroyed_;
};

class Credentials final : public aegra::ports::ICredentialResolver {
  public:
    base::Result<std::unique_ptr<aegra::ports::IResolvedSecret>>
    resolve(const contracts::SecretRef&, const base::CancellationToken&) override {
        ++calls;
        std::unique_ptr<aegra::ports::IResolvedSecret> secret =
            std::make_unique<Secret>(destroyed);
        return base::Result<std::unique_ptr<aegra::ports::IResolvedSecret>>::success(
            std::move(secret));
    }

    std::size_t calls{0};
    bool destroyed{false};
};

class UnusedRandom final : public aegra::ports::IRandomSource {
  public:
    base::Result<void> fill(std::span<std::byte>, const base::CancellationToken&) override {
        return base::Result<void>::failure({base::ErrorCode::kInternal, "random unused"});
    }
};

class Clock final : public aegra::ports::IClock {
  public:
    std::int64_t now_utc_ms() const noexcept override { return 1'000; }
};

class Backend final : public detail::IPersonalArchiveRestoreTaskBackend {
  public:
    base::Result<aegra::pipeline::RestoreSummary>
    run(const detail::PersonalArchiveRestoreBackendRequest& request,
        const base::CancellationToken&) override {
        ++calls;
        source = request.source;
        target = request.target;
        password = request.password;
        memory_budget = request.plan.memory_budget_bytes;
        if (failure) {
            return base::Result<aegra::pipeline::RestoreSummary>::failure(*failure);
        }
        return base::Result<aegra::pipeline::RestoreSummary>::success({100, 4, 64});
    }

    std::size_t calls{0};
    std::filesystem::path source;
    std::filesystem::path target;
    std::string password;
    std::size_t memory_budget{0};
    std::optional<base::Error> failure;
};

contracts::JobRequest valid_job() {
    contracts::JobRequest job;
    job.job_id = "restore-job";
    job.tenant_id = "personal";
    job.operation = contracts::JobOperation::kRestore;
    job.source_refs = {R"(D:\Backups\source.bkf)"};
    job.target_ref = R"(\\?\Volume{01234567-89ab-cdef-0123-456789abcdef}\)";
    job.credential_refs = {contracts::SecretRef{"secret://restore"}};
    job.trace_id = "restore-trace";
    return job;
}

app::WindowsPersonalBackupTaskOptions options() {
    app::WindowsPersonalBackupTaskOptions value;
    value.chunk_size_bytes = 1024;
    value.memory_budget_bytes = 4096;
    return value;
}

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

bool test_success() {
    Credentials credentials;
    UnusedRandom random;
    Clock clock;
    Backend backend;
    app::WindowsPersonalBackupTaskContext context{credentials, random, clock, nullptr};
    auto result = detail::execute_personal_archive_restore_task_with_backend(
        valid_job(), options(), context, {}, backend);
    return expect(result && result.value().message_code == "restore.completed" &&
                      result.value().logical_bytes == 100 && result.value().chunk_count == 4 &&
                      backend.calls == 1 && backend.password == "restore-password" &&
                      backend.memory_budget == 4096 && credentials.destroyed,
                  "restore task maps request, metrics and credential lifetime");
}

bool test_rejection() {
    Credentials credentials;
    UnusedRandom random;
    Clock clock;
    Backend backend;
    app::WindowsPersonalBackupTaskContext context{credentials, random, clock, nullptr};
    auto invalid = valid_job();
    invalid.source_refs.push_back("second.bkf");
    auto result = detail::execute_personal_archive_restore_task_with_backend(
        invalid, options(), context, {}, backend);
    return expect(!result && credentials.calls == 0 && backend.calls == 0,
                  "invalid restore is rejected before credential or target access");
}

bool test_sanitized_failure_and_cancellation() {
    Credentials credentials;
    UnusedRandom random;
    Clock clock;
    Backend backend;
    backend.failure = base::Error{base::ErrorCode::kInsufficientSpace,
                                  "target path and restore-password"};
    app::WindowsPersonalBackupTaskContext context{credentials, random, clock, nullptr};
    auto failed = detail::execute_personal_archive_restore_task_with_backend(
        valid_job(), options(), context, {}, backend);
    bool passed = expect(failed && failed.value().message_code == "restore.target_too_small" &&
                                     failed.value().message_code.find("password") ==
                                         std::string::npos,
                         "restore backend detail is replaced with a stable code");

    aegra::base::CancellationSource cancellation;
    cancellation.request_stop();
    Backend unused;
    auto cancelled = detail::execute_personal_archive_restore_task_with_backend(
        valid_job(), options(), context, cancellation.get_token(), unused);
    passed &= expect(cancelled && cancelled.value().message_code == "restore.cancelled" &&
                         unused.calls == 0,
                     "pre-cancelled restore never accesses the target backend");
    return passed;
}

} // namespace

int main() noexcept {
    try {
        return test_success() && test_rejection() && test_sanitized_failure_and_cancellation()
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
