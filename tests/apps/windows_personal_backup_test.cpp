#include "aegra/apps/worker/windows_personal_backup.h"

#include "windows_personal_backup_runtime.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

namespace app = aegra::apps::worker;
namespace detail = aegra::apps::worker::detail;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

struct RuntimeState final {
    std::size_t prepare_count{0};
    std::size_t create_archive_count{0};
    std::size_t close_count{0};
    std::size_t commit_count{0};
    std::size_t abort_count{0};
    bool source_destroyed{false};
    bool close_after_source_destroyed{false};
    bool fail_prepare{false};
    bool fail_archive_create{false};
    bool fail_source_read{false};
    bool fail_close{false};
    std::optional<aegra::format::Manifest> manifest;
};

class TestBlockSource final : public aegra::ports::IBlockSource {
  public:
    TestBlockSource(std::shared_ptr<RuntimeState> state, std::vector<std::byte> data)
        : state_(std::move(state)), data_(std::move(data)) {}

    ~TestBlockSource() override { state_->source_destroyed = true; }

    TestBlockSource(const TestBlockSource&) = delete;
    TestBlockSource& operator=(const TestBlockSource&) = delete;
    TestBlockSource(TestBlockSource&&) = delete;
    TestBlockSource& operator=(TestBlockSource&&) = delete;

    [[nodiscard]] std::uint64_t size_bytes() const noexcept override { return data_.size(); }

    [[nodiscard]] aegra::base::Result<std::size_t>
    read(const std::uint64_t offset, const std::span<std::byte> destination,
         const aegra::base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return aegra::base::Result<std::size_t>::failure(
                {aegra::base::ErrorCode::kCancelled, "test read cancelled"});
        }
        if (state_->fail_source_read) {
            return aegra::base::Result<std::size_t>::failure(
                {aegra::base::ErrorCode::kIoFailure, "injected source failure"});
        }
        if (offset > data_.size()) {
            return aegra::base::Result<std::size_t>::failure(
                {aegra::base::ErrorCode::kInvalidArgument, "test read out of range"});
        }
        const auto count =
            (std::min)(destination.size(), data_.size() - static_cast<std::size_t>(offset));
        const auto source =
            std::span<const std::byte>(data_).subspan(static_cast<std::size_t>(offset), count);
        std::ranges::copy(source, destination.begin());
        return aegra::base::Result<std::size_t>::success(count);
    }

  private:
    std::shared_ptr<RuntimeState> state_;
    std::vector<std::byte> data_;
};

class TestSnapshotLease final : public detail::ISnapshotLease {
  public:
    explicit TestSnapshotLease(std::shared_ptr<RuntimeState> state) : state_(std::move(state)) {}

    [[nodiscard]] aegra::base::Result<void> close() override {
        ++state_->close_count;
        state_->close_after_source_destroyed = state_->source_destroyed;
        if (state_->fail_close) {
            return aegra::base::Result<void>::failure(
                {aegra::base::ErrorCode::kIoFailure, "injected snapshot close failure"});
        }
        return aegra::base::Result<void>::success();
    }

  private:
    std::shared_ptr<RuntimeState> state_;
};

class TestBackupSession final : public aegra::ports::IBackupSession {
  public:
    explicit TestBackupSession(std::shared_ptr<RuntimeState> state) : state_(std::move(state)) {}

    [[nodiscard]] aegra::base::Result<void>
    write_chunk(const aegra::ports::ChunkWriteRequest&,
                aegra::base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return aegra::base::Result<void>::failure(
                {aegra::base::ErrorCode::kCancelled, "test write cancelled"});
        }
        return aegra::base::Result<void>::success();
    }

    [[nodiscard]] aegra::base::Result<void>
    commit(aegra::base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return aegra::base::Result<void>::failure(
                {aegra::base::ErrorCode::kCancelled, "test commit cancelled"});
        }
        ++state_->commit_count;
        return aegra::base::Result<void>::success();
    }

    void abort() noexcept override { ++state_->abort_count; }

  private:
    std::shared_ptr<RuntimeState> state_;
};

class TestRuntime final : public detail::IWindowsPersonalBackupRuntime {
  public:
    explicit TestRuntime(std::shared_ptr<RuntimeState> state) : state_(std::move(state)) {}

    [[nodiscard]] aegra::base::Result<detail::PreparedVolumeSource>
    prepare_source(const std::filesystem::path& volume_guid_path,
                   const aegra::base::CancellationToken&) override {
        ++state_->prepare_count;
        if (state_->fail_prepare) {
            return aegra::base::Result<detail::PreparedVolumeSource>::failure(
                {aegra::base::ErrorCode::kNotFound, "injected inventory failure"});
        }
        detail::PreparedVolumeMetadata metadata{
            volume_guid_path, {LR"(C:\)"}, "NTFS", "System", 10, 4096,
        };
        std::vector<std::byte> data(10, std::byte{0x2A});
        return aegra::base::Result<detail::PreparedVolumeSource>::success(
            detail::PreparedVolumeSource{
                std::move(metadata),
                std::make_unique<TestSnapshotLease>(state_),
                std::make_unique<TestBlockSource>(state_, std::move(data)),
            });
    }

    [[nodiscard]] aegra::base::Result<std::unique_ptr<aegra::ports::IBackupSession>>
    create_archive(const app::WindowsPersonalVolumeBackupRequest&,
                   const aegra::format::Manifest& manifest) override {
        ++state_->create_archive_count;
        state_->manifest = manifest;
        if (state_->fail_archive_create) {
            return aegra::base::Result<std::unique_ptr<aegra::ports::IBackupSession>>::failure(
                {aegra::base::ErrorCode::kIoFailure, "injected archive create failure"});
        }
        std::unique_ptr<aegra::ports::IBackupSession> session =
            std::make_unique<TestBackupSession>(state_);
        return aegra::base::Result<std::unique_ptr<aegra::ports::IBackupSession>>::success(
            std::move(session));
    }

  private:
    std::shared_ptr<RuntimeState> state_;
};

app::WindowsPersonalVolumeBackupRequest request() {
    app::WindowsPersonalVolumeBackupRequest result;
    result.job_id = "windows-volume-backup";
    result.trace_id = "trace-windows-volume-backup";
    result.volume_guid_path = LR"(\\?\Volume{01234567-89ab-cdef-0123-456789abcdef}\)";
    result.destination = "backup.bkf";
    result.password = "test-password";
    result.file_uuid.front() = std::byte{0x11};
    result.backup_set_uuid.front() = std::byte{0x22};
    result.block_size_bytes = 4;
    result.chunk_size_bytes = 8;
    result.memory_budget_bytes = 16;
    result.created_utc = "2026-08-02T12:00:00Z";
    result.application_version = "0.1.0";
    result.hostname = "test-host";
    return result;
}

bool test_successful_composition() {
    auto state = std::make_shared<RuntimeState>();
    TestRuntime runtime(state);
    auto result =
        detail::backup_windows_personal_volume_with_runtime(request(), {}, nullptr, runtime);
    bool passed = expect(result && result.value().backup.logical_bytes == 10,
                         "composition backs up the prepared logical source");
    passed &= expect(state->prepare_count == 1 && state->create_archive_count == 1 &&
                         state->commit_count == 1,
                     "composition prepares, writes and commits exactly once");
    passed &= expect(state->close_count == 1 && state->close_after_source_destroyed,
                     "block source closes before snapshot deletion");
    passed &= expect(result && !result.value().snapshot_cleanup_error,
                     "successful snapshot cleanup has no warning");
    if (state->manifest) {
        const auto& volume = state->manifest->volumes.front();
        passed &= expect(volume.vss_required && volume.vss_used && volume.total_size == 10,
                         "manifest records the VSS-backed source");
    } else {
        passed &= expect(false, "composition supplies archive manifest");
    }
    return passed;
}

bool test_cleanup_warning_after_commit() {
    auto state = std::make_shared<RuntimeState>();
    state->fail_close = true;
    TestRuntime runtime(state);
    auto result =
        detail::backup_windows_personal_volume_with_runtime(request(), {}, nullptr, runtime);
    return expect(result && state->commit_count == 1, "committed archive remains successful") &&
           expect(result && result.value().snapshot_cleanup_error.has_value(),
                  "snapshot cleanup failure is returned separately");
}

bool test_failure_cleanup() {
    auto state = std::make_shared<RuntimeState>();
    state->fail_source_read = true;
    TestRuntime runtime(state);
    auto result =
        detail::backup_windows_personal_volume_with_runtime(request(), {}, nullptr, runtime);
    bool passed = expect(!result && result.error().code == aegra::base::ErrorCode::kIoFailure,
                         "pipeline source failure is preserved");
    passed &= expect(state->abort_count == 1 && state->close_count == 1,
                     "pipeline failure aborts archive and closes snapshot");
    passed &= expect(state->close_after_source_destroyed,
                     "failure path also closes source before snapshot");

    auto archive_state = std::make_shared<RuntimeState>();
    archive_state->fail_archive_create = true;
    TestRuntime archive_runtime(archive_state);
    auto archive_result = detail::backup_windows_personal_volume_with_runtime(
        request(), {}, nullptr, archive_runtime);
    passed &= expect(!archive_result && archive_state->close_count == 1,
                     "archive creation failure releases prepared snapshot");
    return passed;
}

bool test_validation_precedes_runtime() {
    auto state = std::make_shared<RuntimeState>();
    TestRuntime runtime(state);
    auto invalid = request();
    invalid.password = {};
    auto result =
        detail::backup_windows_personal_volume_with_runtime(invalid, {}, nullptr, runtime);
    return expect(!result && result.error().code == aegra::base::ErrorCode::kInvalidArgument,
                  "invalid request is rejected") &&
           expect(state->prepare_count == 0, "invalid request does not acquire a snapshot");
}

int run_tests() {
    const bool passed = test_successful_composition() && test_cleanup_warning_after_commit() &&
                        test_failure_cleanup() && test_validation_precedes_runtime();
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
