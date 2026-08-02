#include "aegra/apps/worker/personal_archive_verify_task.h"

#include "personal_archive_verify_task_backend.h"

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
    std::string_view view() const noexcept override { return "verify-password"; }

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

class Backend final : public detail::IPersonalArchiveVerifyTaskBackend {
  public:
    base::Result<aegra::pipeline::VerifySummary>
    run(const std::filesystem::path& source, const std::string_view password,
        const aegra::pipeline::VerifyPlan&, const app::WindowsPersonalBackupTaskOptions&,
        const base::CancellationToken&, aegra::ports::IProgressSink*) override {
        ++calls;
        received_source = source;
        received_password = password;
        if (failure) {
            return base::Result<aegra::pipeline::VerifySummary>::failure(*failure);
        }
        return base::Result<aegra::pipeline::VerifySummary>::success({100, 60, 3});
    }

    std::size_t calls{0};
    std::filesystem::path received_source;
    std::string received_password;
    std::optional<base::Error> failure;
};

contracts::JobRequest valid_job() {
    contracts::JobRequest job;
    job.job_id = "verify-job";
    job.tenant_id = "personal";
    job.operation = contracts::JobOperation::kVerify;
    job.source_refs = {R"(D:\Backups\source.bkf)"};
    job.credential_refs = {contracts::SecretRef{"secret://verify"}};
    job.trace_id = "verify-trace";
    return job;
}

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

bool test_success_and_secret_lifetime() {
    Credentials credentials;
    UnusedRandom random;
    Clock clock;
    Backend backend;
    app::WindowsPersonalBackupTaskOptions options;
    options.memory_budget_bytes = 1024;
    app::WindowsPersonalBackupTaskContext context{credentials, random, clock, nullptr};
    auto result = detail::execute_personal_archive_verify_task_with_backend(
        valid_job(), options, context, {}, backend);
    return expect(result && result.value().message_code == "verify.completed" &&
                      result.value().logical_bytes == 100 && result.value().stored_bytes == 60 &&
                      result.value().chunk_count == 3 && credentials.calls == 1 &&
                      credentials.destroyed && backend.received_password == "verify-password",
                  "verify task maps metrics and releases its credential");
}

bool test_rejection_and_sanitized_failure() {
    Credentials credentials;
    UnusedRandom random;
    Clock clock;
    Backend backend;
    app::WindowsPersonalBackupTaskOptions options;
    options.memory_budget_bytes = 1024;
    app::WindowsPersonalBackupTaskContext context{credentials, random, clock, nullptr};
    auto invalid = valid_job();
    invalid.target_ref = "forbidden-target";
    auto rejected = detail::execute_personal_archive_verify_task_with_backend(
        invalid, options, context, {}, backend);
    bool passed = expect(!rejected && credentials.calls == 0 && backend.calls == 0,
                         "invalid verify task is rejected before dependency access");

    backend.failure = base::Error{base::ErrorCode::kCorruptData,
                                  "D:\\customer\\secret and verify-password"};
    auto failed = detail::execute_personal_archive_verify_task_with_backend(
        valid_job(), options, context, {}, backend);
    passed &= expect(failed && failed.value().message_code == "verify.corrupt" &&
                         failed.value().message_code.find("customer") == std::string::npos,
                     "verify task replaces backend detail with a stable code");
    return passed;
}

} // namespace

int main() noexcept {
    try {
        return test_success_and_secret_lifetime() && test_rejection_and_sanitized_failure()
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
