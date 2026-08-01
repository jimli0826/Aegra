#include "aegra/contracts/job.h"

#include <cstdlib>
#include <cstdio>

namespace {

aegra::contracts::JobRequest valid_request() {
    aegra::contracts::JobRequest request;
    request.job_id = "job-1";
    request.tenant_id = "tenant-1";
    request.source_refs = {"disk-0"};
    request.target_ref = "repository-1";
    request.trace_id = "trace-1";
    return request;
}

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

int run_tests() {
    auto request = valid_request();
    bool passed = expect(aegra::contracts::validate_job_request(request).has_value(),
                         "valid job is accepted");

    request.schema_version = 99;
    const auto unsupported = aegra::contracts::validate_job_request(request);
    passed &= expect(!unsupported.has_value(), "unsupported schema is rejected");
    passed &= expect(
        unsupported.error().code == aegra::base::ErrorCode::kUnsupportedVersion,
        "unsupported schema has stable error code");

    request = valid_request();
    request.job_id.clear();
    passed &= expect(!aegra::contracts::validate_job_request(request).has_value(),
                     "missing job id is rejected");
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
