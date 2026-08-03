#include "aegra/base/error.h"
#include "aegra/base/result.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

int run_tests() {
    auto value = aegra::base::Result<int>::success(42);
    auto failure = aegra::base::Result<int>::failure(
        {aegra::base::ErrorCode::kInvalidArgument, "invalid input"});
    auto empty = aegra::base::Result<void>::success();

    bool passed = expect(value.has_value() && value.value() == 42, "result stores value");
    passed &= expect(!failure.has_value(), "result stores failure");
    passed &= expect(failure.error().code == aegra::base::ErrorCode::kInvalidArgument,
                     "result exposes stable error code");
    passed &= expect(empty.has_value(), "void result stores success");
    passed &= expect(aegra::base::error_code_name(failure.error().code) == "invalid_argument",
                     "error code has stable name");
    passed &= expect(aegra::base::error_code_name(aegra::base::ErrorCode::kInsufficientSpace) ==
                         "insufficient_space",
                     "insufficient space error code has stable name");
    passed &= expect(aegra::base::error_code_name(aegra::base::ErrorCode::kOutcomeUnknown) ==
                         "outcome_unknown",
                     "unknown outcome error code has stable name");
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
