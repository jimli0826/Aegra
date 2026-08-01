#include "aegra/adapters/memory/memory_backup_store.h"
#include "aegra/adapters/memory/memory_block_io.h"
#include "aegra/contracts/progress.h"
#include "aegra/pipeline/backup_pipeline.h"
#include "aegra/pipeline/fixed_size_chunker.h"
#include "aegra/pipeline/restore_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

namespace {

using aegra::adapters::memory::MemoryBackupStore;
using aegra::adapters::memory::MemoryBackupStoreOptions;
using aegra::adapters::memory::MemoryBlockSink;
using aegra::adapters::memory::MemoryBlockSinkOptions;
using aegra::adapters::memory::MemoryBlockSource;
using aegra::adapters::memory::MemoryBlockSourceOptions;
using aegra::pipeline::BackupPipeline;
using aegra::pipeline::BackupPlan;
using aegra::pipeline::RestorePipeline;
using aegra::pipeline::RestorePlan;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

std::vector<std::byte> make_data(const std::size_t size) {
    std::vector<std::byte> result(size);
    for (std::size_t index = 0; index < size; ++index) {
        result[index] = static_cast<std::byte>((index * 37 + 11) % 251);
    }
    return result;
}

class CollectingProgressSink final : public aegra::ports::IProgressSink {
  public:
    void publish(const aegra::contracts::TaskProgress& progress) noexcept override {
        ++event_count;
        last_phase = progress.phase;
        last_logical_bytes = progress.logical_bytes;
        last_processed_bytes = progress.processed_bytes;
    }

    std::size_t event_count{0};
    aegra::contracts::TaskPhase last_phase{aegra::contracts::TaskPhase::kUnspecified};
    std::uint64_t last_logical_bytes{0};
    std::uint64_t last_processed_bytes{0};
};

class CorruptReader final : public aegra::ports::IRecoveryPointReader {
  public:
    [[nodiscard]] std::uint64_t logical_size_bytes() const noexcept override { return 4; }

    [[nodiscard]] std::uint64_t chunk_count() const noexcept override { return 1; }

    [[nodiscard]] aegra::base::Result<aegra::ports::ChunkDescriptor>
    describe_chunk(std::uint64_t) const override {
        return aegra::base::Result<aegra::ports::ChunkDescriptor>::success(
            aegra::ports::ChunkDescriptor{0, 1, 3, 3});
    }

    [[nodiscard]] aegra::base::Result<aegra::ports::ChunkData>
    read_chunk(std::uint64_t, aegra::base::CancellationToken) override {
        return aegra::base::Result<aegra::ports::ChunkData>::success(
            aegra::ports::ChunkData{{0, 1, 3, 3}, make_data(3)});
    }
};

bool range_has_size(const aegra::base::Result<std::optional<aegra::pipeline::ChunkRange>>& result,
                    const std::uint64_t expected_size) {
    if (!result || !result.value().has_value()) {
        return false;
    }
    return result.value().value().logical_size == expected_size;
}

bool chunker_contract() {
    auto invalid = aegra::pipeline::FixedSizeChunker::create(0);
    bool passed = expect(!invalid, "chunker rejects a zero chunk size");
    auto chunker = aegra::pipeline::FixedSizeChunker::create(4);
    if (!chunker) {
        return false;
    }
    auto first = chunker.value().next(10, 0, 0);
    auto tail = chunker.value().next(10, 8, 2);
    auto end = chunker.value().next(10, 10, 3);
    passed &= expect(range_has_size(first, 4), "chunker emits a full first range");
    passed &= expect(range_has_size(tail, 2), "chunker emits a short tail range");
    passed &= expect(end && !end.value(), "chunker ends exactly at source size");
    return passed;
}

bool roundtrip_with_backpressure() {
    const auto input = make_data(10'123);
    MemoryBlockSource source(input, MemoryBlockSourceOptions{17, std::nullopt});
    MemoryBackupStore store;
    auto session = store.create_session(input.size());
    if (!session) {
        return false;
    }

    CollectingProgressSink backup_progress;
    BackupPipeline backup(source, *session.value(), &backup_progress);
    auto backup_result = backup.run(BackupPlan{"backup-roundtrip", 1024, 1024}, {});
    bool passed = expect(backup_result.has_value(), "backup pipeline completes");
    if (!backup_result) {
        return false;
    }
    passed &= expect(backup_result.value().chunk_count == 10,
                     "backup emits deterministic fixed-size chunks");
    passed &= expect(backup_result.value().peak_buffered_bytes <= 1024,
                     "backup queue stays within its byte budget");
    passed &= expect(store.is_committed(), "backup publishes the memory recovery point");
    passed &= expect(backup_progress.event_count > 0 &&
                         backup_progress.last_phase == aegra::contracts::TaskPhase::kCompleted,
                     "backup publishes completed progress");

    auto reader = store.open_reader();
    if (!reader) {
        return false;
    }
    MemoryBlockSink sink(input.size());
    CollectingProgressSink restore_progress;
    RestorePipeline restore(*reader.value(), sink, &restore_progress);
    auto restore_result = restore.run(RestorePlan{"restore-roundtrip", 1024}, {});
    passed &= expect(restore_result.has_value(), "restore pipeline completes");
    passed &= expect(restore_result && restore_result.value().peak_buffered_bytes <= 1024,
                     "restore queue stays within its byte budget");
    passed &= expect(sink.snapshot() == input, "restored bytes equal the source exactly");
    passed &= expect(sink.flush_count() == 1, "successful restore flushes once");
    passed &= expect(restore_progress.event_count > 0 &&
                         restore_progress.last_phase == aegra::contracts::TaskPhase::kCompleted &&
                         restore_progress.last_logical_bytes == input.size() &&
                         restore_progress.last_processed_bytes == input.size(),
                     "restore publishes valid completed progress");
    return passed;
}

bool empty_roundtrip() {
    MemoryBlockSource source({});
    MemoryBackupStore store;
    auto session = store.create_session(0);
    BackupPipeline backup(source, *session.value());
    auto backup_result = backup.run(BackupPlan{"empty-backup", 16, 16}, {});
    bool passed = expect(backup_result && backup_result.value().chunk_count == 0,
                         "empty backup commits without chunks");

    auto reader = store.open_reader();
    MemoryBlockSink sink(0);
    RestorePipeline restore(*reader.value(), sink);
    auto restore_result = restore.run(RestorePlan{"empty-restore", 16}, {});
    passed &= expect(restore_result && restore_result.value().restored_bytes == 0,
                     "empty recovery point restores successfully");
    return passed;
}

bool backup_failure_paths() {
    const auto input = make_data(4096);
    MemoryBlockSource failing_source(input, MemoryBlockSourceOptions{1024, 1500});
    MemoryBackupStore failed_store;
    auto failed_session = failed_store.create_session(input.size());
    BackupPipeline failing_backup(failing_source, *failed_session.value());
    auto read_failure = failing_backup.run(BackupPlan{"read-failure", 1024, 1024}, {});
    bool passed = expect(!read_failure && failed_store.is_aborted(),
                         "read failure aborts and hides the recovery point");

    MemoryBlockSource source(input);
    MemoryBackupStore commit_store(MemoryBackupStoreOptions{std::nullopt, true});
    auto commit_session = commit_store.create_session(input.size());
    BackupPipeline commit_failure(source, *commit_session.value());
    auto commit_result = commit_failure.run(BackupPlan{"commit-failure", 1024, 2048}, {});
    passed &= expect(!commit_result && commit_store.is_aborted(),
                     "commit failure aborts the backup session");

    MemoryBackupStore cancelled_store;
    auto cancelled_session = cancelled_store.create_session(input.size());
    BackupPipeline cancelled_backup(source, *cancelled_session.value());
    aegra::base::CancellationSource cancellation;
    cancellation.request_stop();
    auto cancelled =
        cancelled_backup.run(BackupPlan{"cancelled-backup", 1024, 1024}, cancellation.get_token());
    passed &= expect(!cancelled && cancelled.error().code == aegra::base::ErrorCode::kCancelled &&
                         cancelled_store.is_aborted(),
                     "cancelled backup responds and aborts");
    return passed;
}

bool restore_preflight_and_failure_paths() {
    const auto input = make_data(2048);
    MemoryBlockSource source(input);
    MemoryBackupStore store;
    auto session = store.create_session(input.size());
    BackupPipeline backup(source, *session.value());
    if (!backup.run(BackupPlan{"preflight-source", 1024, 1024}, {})) {
        return false;
    }
    auto reader = store.open_reader();

    MemoryBlockSink too_small(input.size() - 1);
    RestorePipeline capacity_restore(*reader.value(), too_small);
    auto capacity = capacity_restore.run(RestorePlan{"capacity", 1024}, {});
    bool passed =
        expect(!capacity && capacity.error().code == aegra::base::ErrorCode::kInsufficientSpace &&
                   too_small.flush_count() == 0,
               "capacity preflight fails before writes and flush");

    MemoryBlockSinkOptions failure_options;
    failure_options.fail_at_offset = 1024;
    MemoryBlockSink failing_sink(input.size(), failure_options);
    RestorePipeline failing_restore(*reader.value(), failing_sink);
    auto write_failure = failing_restore.run(RestorePlan{"write-failure", 1024}, {});
    passed &=
        expect(!write_failure && write_failure.error().code == aegra::base::ErrorCode::kIoFailure &&
                   failing_sink.flush_count() == 0,
               "restore reports write failure without flushing partial output");

    CorruptReader corrupt_reader;
    MemoryBlockSink untouched_sink(4);
    RestorePipeline corrupt_restore(corrupt_reader, untouched_sink);
    auto corrupt = corrupt_restore.run(RestorePlan{"corrupt", 4}, {});
    passed &= expect(!corrupt && corrupt.error().code == aegra::base::ErrorCode::kCorruptData &&
                         untouched_sink.flush_count() == 0,
                     "corrupt mapping is rejected before destructive writes");
    return passed;
}

int run_tests() {
    bool passed = chunker_contract();
    passed &= roundtrip_with_backpressure();
    passed &= empty_roundtrip();
    passed &= backup_failure_paths();
    passed &= restore_preflight_and_failure_paths();
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
