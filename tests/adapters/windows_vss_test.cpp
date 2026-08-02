#include "aegra/adapters/windows_vss/windows_vss.h"

#include "vss_snapshot_core.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

namespace windows_vss = aegra::adapters::windows_vss;
namespace detail = aegra::adapters::windows_vss::detail;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

struct FakeState final {
    std::size_t create_count{0};
    std::size_t close_count{0};
    std::size_t abandon_count{0};
    bool fail_close{false};
    bool inconsistent_mapping{false};
};

class FakeVssBackend final : public detail::IVssSnapshotBackend {
  public:
    explicit FakeVssBackend(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

    [[nodiscard]] aegra::base::Result<std::vector<windows_vss::WindowsVssSnapshot>>
    create(const std::span<const windows_vss::WindowsVssSnapshotRequest> requests,
           const aegra::base::CancellationToken&) override {
        ++state_->create_count;
        std::vector<windows_vss::WindowsVssSnapshot> snapshots;
        snapshots.reserve(requests.size());
        for (const auto& request : requests) {
            snapshots.push_back(windows_vss::WindowsVssSnapshot{
                request.volume_guid_path,
                LR"(\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy42)",
                request.logical_size_bytes,
            });
        }
        if (state_->inconsistent_mapping) {
            snapshots.front().logical_size_bytes += 1U;
        }
        return aegra::base::Result<std::vector<windows_vss::WindowsVssSnapshot>>::success(
            std::move(snapshots));
    }

    [[nodiscard]] aegra::base::Result<void> close() override {
        ++state_->close_count;
        if (state_->fail_close) {
            return aegra::base::Result<void>::failure(
                aegra::base::Error{aegra::base::ErrorCode::kIoFailure, "injected close failure"});
        }
        return aegra::base::Result<void>::success();
    }

    void abandon() noexcept override { ++state_->abandon_count; }

  private:
    std::shared_ptr<FakeState> state_;
};

windows_vss::WindowsVssSnapshotRequest request() {
    return windows_vss::WindowsVssSnapshotRequest{
        LR"(\\?\Volume{01234567-89ab-cdef-0123-456789abcdef}\)",
        4096,
    };
}

std::unique_ptr<detail::IVssSnapshotBackend> backend(const std::shared_ptr<FakeState>& state) {
    return std::make_unique<FakeVssBackend>(state);
}

bool test_path_validation() {
    using windows_vss::WindowsVssSnapshotSession;
    bool passed =
        expect(WindowsVssSnapshotSession::is_canonical_volume_guid_path(request().volume_guid_path),
               "canonical Volume GUID path is accepted");
    passed &= expect(!WindowsVssSnapshotSession::is_canonical_volume_guid_path(
                         LR"(\\?\Volume{01234567-89ab-cdef-0123-456789abcdef})"),
                     "Volume GUID path requires a trailing separator");
    passed &= expect(!WindowsVssSnapshotSession::is_canonical_volume_guid_path(LR"(C:\)"),
                     "drive mount point is not a canonical Volume GUID path");
    passed &= expect(!WindowsVssSnapshotSession::is_canonical_volume_guid_path(
                         LR"(\\?\Volume{01234567-89ab-cdef-0123-456789abcdeg}\)"),
                     "Volume GUID text rejects non-hex characters");
    return passed;
}

bool test_request_validation() {
    auto state = std::make_shared<FakeState>();
    std::vector<windows_vss::WindowsVssSnapshotRequest> empty;
    auto empty_result = detail::VssSnapshotSessionCore::create(empty, {}, backend(state));
    bool passed = expect(!empty_result &&
                             empty_result.error().code == aegra::base::ErrorCode::kInvalidArgument,
                         "empty snapshot set is rejected");

    auto zero_size = request();
    zero_size.logical_size_bytes = 0;
    auto zero_result = detail::VssSnapshotSessionCore::create(
        std::span<const windows_vss::WindowsVssSnapshotRequest>(&zero_size, 1), {}, backend(state));
    passed &=
        expect(!zero_result && zero_result.error().code == aegra::base::ErrorCode::kInvalidArgument,
               "zero-sized volume is rejected");

    const std::vector duplicates{request(), request()};
    auto duplicate_result = detail::VssSnapshotSessionCore::create(duplicates, {}, backend(state));
    passed &= expect(!duplicate_result &&
                         duplicate_result.error().code == aegra::base::ErrorCode::kConflict,
                     "duplicate volume identity is rejected");
    passed &= expect(state->create_count == 0, "invalid requests never invoke VSS backend");
    return passed;
}

bool test_cancellation() {
    auto state = std::make_shared<FakeState>();
    aegra::base::CancellationSource cancellation;
    cancellation.request_stop();
    const auto volume = request();
    auto result = detail::VssSnapshotSessionCore::create(
        std::span<const windows_vss::WindowsVssSnapshotRequest>(&volume, 1),
        cancellation.get_token(), backend(state));
    return expect(!result && result.error().code == aegra::base::ErrorCode::kCancelled,
                  "pre-cancelled creation is rejected") &&
           expect(state->create_count == 0, "pre-cancellation avoids VSS backend work");
}

bool test_success_and_idempotent_close() {
    auto state = std::make_shared<FakeState>();
    const auto volume = request();
    auto created = detail::VssSnapshotSessionCore::create(
        std::span<const windows_vss::WindowsVssSnapshotRequest>(&volume, 1), {}, backend(state));
    bool passed = expect(created && created.value()->active(), "created session is active");
    if (!created) {
        return false;
    }
    passed &= expect(created.value()->snapshots().size() == 1,
                     "session exposes one snapshot mapping per request");
    passed &= expect(created.value()->close().has_value(), "explicit close succeeds");
    passed &= expect(!created.value()->active(), "closed session becomes inactive");
    passed &= expect(created.value()->close().has_value(), "close is idempotent");
    created.value().reset();
    passed &= expect(state->close_count == 1 && state->abandon_count == 0,
                     "successful close does not run destructor cleanup again");
    return passed;
}

bool test_failure_and_destructor_cleanup() {
    auto state = std::make_shared<FakeState>();
    state->fail_close = true;
    const auto volume = request();
    auto created = detail::VssSnapshotSessionCore::create(
        std::span<const windows_vss::WindowsVssSnapshotRequest>(&volume, 1), {}, backend(state));
    if (!created) {
        return false;
    }
    bool passed = expect(!created.value()->close(), "close failure is reported");
    passed &=
        expect(created.value()->active(), "failed close leaves cleanup responsibility active");
    created.value().reset();
    passed &= expect(state->abandon_count == 1, "destructor retries cleanup after close failure");

    auto inconsistent_state = std::make_shared<FakeState>();
    inconsistent_state->inconsistent_mapping = true;
    auto inconsistent = detail::VssSnapshotSessionCore::create(
        std::span<const windows_vss::WindowsVssSnapshotRequest>(&volume, 1), {},
        backend(inconsistent_state));
    passed &=
        expect(!inconsistent && inconsistent.error().code == aegra::base::ErrorCode::kInternal,
               "inconsistent backend mapping is rejected");
    passed &= expect(inconsistent_state->abandon_count == 1,
                     "inconsistent mapping triggers immediate cleanup");
    return passed;
}

int run_tests() {
    const bool passed = test_path_validation() && test_request_validation() &&
                        test_cancellation() && test_success_and_idempotent_close() &&
                        test_failure_and_destructor_cleanup();
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
