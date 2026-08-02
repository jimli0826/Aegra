#include "aegra/adapters/windows_system/windows_system.h"

#include "aegra/base/error.h"
#include "aegra/contracts/job.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

namespace windows_system = aegra::adapters::windows_system;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

bool test_clock() {
    windows_system::WindowsSystemClock clock;
    const auto now = clock.now_utc_ms();
    const auto standard_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
    const auto difference = now > standard_now ? now - standard_now : standard_now - now;
    return expect(now > 0 && difference < 5'000, "Windows clock reports current Unix UTC ms");
}

bool test_random() {
    windows_system::WindowsCryptographicRandom random;
    std::array<std::byte, 32> bytes{};
    bool passed = expect(random.fill(bytes, {}).has_value(),
                         "Windows cryptographic random fills a destination");
    passed &= expect(random.fill({}, {}).has_value(), "empty random request succeeds");

    aegra::base::CancellationSource cancellation;
    cancellation.request_stop();
    const auto cancelled = random.fill(bytes, cancellation.get_token());
    passed &= expect(!cancelled && cancelled.error().code == aegra::base::ErrorCode::kCancelled,
                     "pre-cancelled random request is rejected");
    return passed;
}

bool test_credential_validation() {
    windows_system::WindowsCredentialResolver resolver;
    auto unsupported = resolver.resolve(aegra::contracts::SecretRef{"secret://wrong"}, {});
    bool passed =
        expect(!unsupported && unsupported.error().code == aegra::base::ErrorCode::kInvalidArgument,
               "credential resolver accepts only wincred references");
    auto empty = resolver.resolve(aegra::contracts::SecretRef{"wincred://"}, {});
    passed &= expect(!empty && empty.error().code == aegra::base::ErrorCode::kInvalidArgument,
                     "empty Windows credential target is rejected");

    aegra::base::CancellationSource cancellation;
    cancellation.request_stop();
    auto cancelled =
        resolver.resolve(aegra::contracts::SecretRef{"wincred://unused"}, cancellation.get_token());
    passed &= expect(!cancelled && cancelled.error().code == aegra::base::ErrorCode::kCancelled,
                     "pre-cancelled credential lookup does not access Credential Manager");
    const auto missing_target = "wincred://aegra-test-target-that-must-not-exist";
    auto missing = resolver.resolve(aegra::contracts::SecretRef{missing_target}, {});
    passed &= expect(!missing && missing.error().code == aegra::base::ErrorCode::kNotFound,
                     "missing Windows credential reports not found without target disclosure");
    return passed;
}

int run_tests() {
    return test_clock() && test_random() && test_credential_validation() ? EXIT_SUCCESS
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
