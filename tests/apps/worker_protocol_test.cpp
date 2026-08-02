#include "aegra/apps/worker/worker_protocol.h"

#include "aegra/base/error.h"
#include "aegra/base/result.h"
#include "aegra/contracts/task_result.h"
#include "aegra/contracts/worker_response.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/credential.h"
#include "aegra/ports/random.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace app = aegra::apps::worker;
namespace base = aegra::base;
namespace contracts = aegra::contracts;

class UnusedCredentials final : public aegra::ports::ICredentialResolver {
  public:
    [[nodiscard]] base::Result<std::unique_ptr<aegra::ports::IResolvedSecret>>
    resolve(const contracts::SecretRef&, const base::CancellationToken&) override {
        ++call_count;
        return base::Result<std::unique_ptr<aegra::ports::IResolvedSecret>>::failure(
            {base::ErrorCode::kInternal, "unexpected credential access"});
    }

    std::size_t call_count{0};
};

class UnusedRandom final : public aegra::ports::IRandomSource {
  public:
    [[nodiscard]] base::Result<void> fill(std::span<std::byte>,
                                          const base::CancellationToken&) override {
        ++call_count;
        return base::Result<void>::failure(
            {base::ErrorCode::kInternal, "unexpected random access"});
    }

    std::size_t call_count{0};
};

class FixedClock final : public aegra::ports::IClock {
  public:
    [[nodiscard]] std::int64_t now_utc_ms() const noexcept override { return 1'000; }
};

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

std::string valid_job_json() {
    return R"({
        "schema_version": 1,
        "job_id": "job-1",
        "tenant_id": "tenant-1",
        "operation": 1,
        "source_refs": ["source-1"],
        "target_ref": "target-1",
        "credential_refs": ["secret://personal/password"],
        "trace_id": "trace-1",
        "deadline_utc_ms": 2000
    })";
}

bool test_decode_job() {
    auto decoded = app::decode_worker_job_request(valid_job_json());
    bool passed =
        expect(decoded && decoded.value().job_id == "job-1" &&
                   decoded.value().credential_refs.size() == 1 &&
                   decoded.value().credential_refs.front().value == "secret://personal/password",
               "valid JSON decodes into an owned job request");
    auto malformed = app::decode_worker_job_request(R"({"schema_version":"1"})");
    passed &= expect(!malformed && malformed.error().code == base::ErrorCode::kInvalidArgument,
                     "malformed JSON fields are rejected with a stable error");

    auto plaintext_json = valid_job_json();
    plaintext_json.insert(plaintext_json.find_last_of('}'), R"(,"password":"forbidden")");
    auto plaintext = app::decode_worker_job_request(plaintext_json);
    passed &= expect(!plaintext && plaintext.error().code == base::ErrorCode::kInvalidArgument,
                     "plaintext password fields are rejected");

    auto overflow_json = valid_job_json();
    const auto operation = overflow_json.find(R"("operation": 1)");
    overflow_json.replace(operation, std::string(R"("operation": 1)").size(),
                          R"("operation": 257)");
    auto overflow = app::decode_worker_job_request(overflow_json);
    passed &= expect(!overflow && overflow.error().code == base::ErrorCode::kInvalidArgument,
                     "out-of-range numeric fields cannot narrow into valid enums");

    auto fractional_json = valid_job_json();
    const auto deadline = fractional_json.find(R"("deadline_utc_ms": 2000)");
    fractional_json.replace(deadline, std::string(R"("deadline_utc_ms": 2000)").size(),
                            R"("deadline_utc_ms": 1.5)");
    passed &= expect(!app::decode_worker_job_request(fractional_json),
                     "fractional deadlines are rejected instead of truncated");

    auto negative_operation_json = valid_job_json();
    const auto negative_operation = negative_operation_json.find(R"("operation": 1)");
    negative_operation_json.replace(negative_operation, std::string(R"("operation": 1)").size(),
                                    R"("operation": -255)");
    passed &= expect(!app::decode_worker_job_request(negative_operation_json),
                     "negative operation values cannot wrap into valid enums");

    auto unsupported = app::decode_worker_job_request(R"({
        "schema_version": 2,
        "job_id": "job-1",
        "tenant_id": "tenant-1",
        "operation": 1,
        "source_refs": ["source-1"],
        "target_ref": "target-1",
        "credential_refs": [],
        "trace_id": "trace-1"
    })");
    passed &=
        expect(!unsupported && unsupported.error().code == base::ErrorCode::kUnsupportedVersion,
               "unsupported request schema preserves its stable error code");
    return passed;
}

contracts::WorkerResponse successful_response() {
    contracts::TaskResult task;
    task.job_id = "job-1";
    task.trace_id = "trace-1";
    task.outcome = contracts::TaskOutcome::kSucceeded;
    task.error_code = base::ErrorCode::kNone;
    task.logical_bytes = 100;
    task.stored_bytes = 80;
    task.chunk_count = 4;
    task.message_code = "backup.completed";

    contracts::WorkerResponse response;
    response.job_id = "job-1";
    response.trace_id = "trace-1";
    response.kind = contracts::WorkerResponseKind::kTaskResult;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "worker.task_finished";
    response.task_result = task;
    return response;
}

bool test_encode_response() {
    auto encoded = app::encode_worker_response(successful_response());
    bool passed = expect(encoded.has_value(), "valid worker response encodes as JSON");
    if (encoded) {
        passed &= expect(encoded.value().find("\"kind\":1") != std::string::npos &&
                             encoded.value().find("\"outcome\":1") != std::string::npos &&
                             encoded.value().find("\"logical_bytes\":100") != std::string::npos,
                         "response JSON uses stable numeric enums and metrics");
        passed &= expect(encoded.value().find("secret://") == std::string::npos,
                         "response JSON contains no credential reference");
    }
    auto invalid = successful_response();
    auto mismatched = contracts::TaskResult{};
    mismatched.job_id = "job-1";
    mismatched.trace_id = "wrong-trace";
    mismatched.outcome = contracts::TaskOutcome::kSucceeded;
    mismatched.error_code = base::ErrorCode::kNone;
    mismatched.message_code = "backup.completed";
    invalid.task_result = std::move(mismatched);
    passed &= expect(!app::encode_worker_response(invalid),
                     "invalid response cannot be serialized across the process boundary");
    return passed;
}

bool test_encoded_request_rejection() {
    UnusedCredentials credentials;
    UnusedRandom random;
    FixedClock clock;
    aegra::apps::worker::WindowsPersonalBackupTaskOptions options;
    const aegra::apps::worker::WindowsPersonalBackupTaskContext context{
        credentials,
        random,
        clock,
        nullptr,
    };
    auto rejected = app::run_windows_personal_backup_worker_request("{}", options, context, {});
    bool passed = expect(
        rejected && rejected.value().exit_code == app::WorkerExitCode::kRequestRejected &&
            rejected.value().response_json.find("worker.request_rejected") != std::string::npos,
        "decode failure returns a structured rejection response");
    passed &= expect(credentials.call_count == 0 && random.call_count == 0,
                     "rejected encoded request acquires no system capabilities");

    const std::string oversized(1024U * 1024U + 1U, 'x');
    rejected = app::run_windows_personal_backup_worker_request(oversized, options, context, {});
    passed &=
        expect(rejected && rejected.value().exit_code == app::WorkerExitCode::kRequestRejected,
               "oversized encoded request is rejected before JSON parsing");
    return passed;
}

int run_tests() {
    return test_decode_job() && test_encode_response() && test_encoded_request_rejection()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
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
