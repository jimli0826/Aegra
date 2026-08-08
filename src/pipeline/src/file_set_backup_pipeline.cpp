#include "aegra/pipeline/file_set_backup_pipeline.h"

#include "aegra/base/error.h"
#include "aegra/format/personal_archive.h"
#include "aegra/pipeline/file_set_change_planner.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aegra::pipeline {
namespace {

constexpr std::uint64_t kMaximumWireInteger =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

[[nodiscard]] base::Error err(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] base::Result<std::uint64_t> checked_add(const std::uint64_t left,
                                                      const std::uint64_t right) {
    if (left > kMaximumWireInteger || right > kMaximumWireInteger ||
        right > kMaximumWireInteger - left) {
        return base::Result<std::uint64_t>::failure(
            err(base::ErrorCode::kInvalidArgument, "file backup counter overflow"));
    }
    return base::Result<std::uint64_t>::success(left + right);
}

class SessionGuard final {
  public:
    explicit SessionGuard(ports::IFileBackupSession& session) noexcept : session_(session) {}
    ~SessionGuard() {
        if (!committed_) {
            session_.abort();
        }
    }
    void mark_committed() noexcept { committed_ = true; }
    SessionGuard(const SessionGuard&) = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;

  private:
    ports::IFileBackupSession& session_;
    bool committed_{false};
};

[[nodiscard]] base::Result<void> validate_plan(const FileSetBackupPlan& plan) {
    if (plan.job_id.empty() || plan.trace_id.empty() || plan.selections.empty()) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kInvalidArgument, "file backup plan is incomplete"));
    }
    if (plan.block_size_bytes < format::personal_archive::kMinimumFileBlockSizeBytes ||
        plan.block_size_bytes % format::personal_archive::kFileBlockSizeAlignment != 0 ||
        plan.chunk_size_bytes < plan.block_size_bytes ||
        plan.chunk_size_bytes % plan.block_size_bytes != 0) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kInvalidArgument, "file backup chunk sizing is invalid"));
    }
    if (plan.memory_budget_bytes < plan.chunk_size_bytes || plan.enumerate_batch_size == 0) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kInsufficientSpace, "file backup memory budget is insufficient"));
    }
    if (plan.effective_type == contracts::BackupType::kIncremental) {
        if (plan.parent_reader == nullptr || plan.parent_checkpoints.empty()) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kInvalidArgument, "file_backup.parent_required"));
        }
        auto checkpoints = contracts::validate_file_journal_checkpoints(plan.parent_checkpoints);
        if (!checkpoints) {
            return base::Result<void>::failure(checkpoints.error());
        }
    } else if (plan.effective_type != contracts::BackupType::kFull) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kInvalidArgument, "file backup type is unsupported"));
    }
    return contracts::validate_file_source_refs(plan.selections);
}

void publish(ports::IProgressSink* sink, const FileSetBackupPlan& plan,
             const contracts::TaskPhase phase, const FileSetBackupSummary& summary,
             const char* message) {
    if (sink == nullptr) {
        return;
    }
    contracts::TaskProgress progress;
    progress.job_id = plan.job_id;
    progress.trace_id = plan.trace_id;
    progress.phase = phase;
    // Progress uses local materialization work only. Incremental tip-visible logical_bytes
    // includes parent-referenced streams that are never written this layer; using that total
    // fails completed validation (processed != logical) and forces worker.progress_failed.
    progress.logical_bytes = summary.progress_logical_bytes == 0
                                 ? std::nullopt
                                 : std::optional(summary.progress_logical_bytes);
    // processed_bytes is logical source progress; stored_bytes may be smaller after zstd.
    progress.processed_bytes = summary.processed_bytes;
    progress.stored_bytes = summary.stored_bytes;
    progress.discovered_entries = summary.entry_count;
    progress.processed_entries = summary.processed_entries;
    progress.message_code = message;
    sink->publish(std::move(progress));
}

[[nodiscard]] base::Result<std::vector<contracts::FileEntryDesc>>
enumerate_selection(ports::IFileSnapshotView& snapshot, const contracts::FileSourceRef& selection,
                    const std::uint32_t batch_size, const base::CancellationToken& cancellation,
                    FileSetBackupSummary& summary) {
    auto enumerator = snapshot.open_enumerator(selection, cancellation);
    if (!enumerator) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(enumerator.error());
    }
    std::vector<contracts::FileEntryDesc> entries;
    while (true) {
        if (cancellation.stop_requested()) {
            return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
                err(base::ErrorCode::kCancelled, "file backup enumeration cancelled"));
        }
        auto batch = enumerator.value()->next_batch(batch_size, cancellation);
        if (!batch) {
            return base::Result<std::vector<contracts::FileEntryDesc>>::failure(batch.error());
        }
        for (auto& entry : batch.value().entries) {
            entries.push_back(std::move(entry));
            auto next = checked_add(summary.entry_count, 1);
            if (!next) {
                return base::Result<std::vector<contracts::FileEntryDesc>>::failure(next.error());
            }
            summary.entry_count = next.value();
        }
        if (!batch.value().continuation_token.has_value()) {
            break;
        }
    }
    return base::Result<std::vector<contracts::FileEntryDesc>>::success(std::move(entries));
}

[[nodiscard]] base::Result<std::vector<contracts::FileEntryDesc>>
enumerate_all(ports::IFileSnapshotView& snapshot, const FileSetBackupPlan& plan,
              const base::CancellationToken& cancellation, FileSetBackupSummary& summary,
              ports::IProgressSink* progress) {
    std::vector<contracts::FileEntryDesc> all;
    for (const auto& selection : plan.selections) {
        auto page =
            enumerate_selection(snapshot, selection, plan.enumerate_batch_size, cancellation, summary);
        if (!page) {
            return page;
        }
        all.insert(all.end(), std::make_move_iterator(page.value().begin()),
                   std::make_move_iterator(page.value().end()));
        publish(progress, plan, contracts::TaskPhase::kPreparing, summary, "file_backup.enumerating");
    }
    if (all.empty()) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
            err(base::ErrorCode::kInvalidArgument, "file backup selection is empty"));
    }
    return base::Result<std::vector<contracts::FileEntryDesc>>::success(std::move(all));
}

[[nodiscard]] std::vector<std::string> unique_volume_identities(
    const std::vector<contracts::FileSourceRef>& selections) {
    std::vector<std::string> volumes;
    for (const auto& selection : selections) {
        volumes.push_back(selection.volume_identity);
    }
    std::sort(volumes.begin(), volumes.end());
    volumes.erase(std::unique(volumes.begin(), volumes.end()), volumes.end());
    return volumes;
}

[[nodiscard]] const contracts::FileJournalCheckpoint*
find_checkpoint(const std::vector<contracts::FileJournalCheckpoint>& checkpoints,
                const std::string& volume_identity) noexcept {
    for (const auto& checkpoint : checkpoints) {
        if (checkpoint.volume_identity == volume_identity) {
            return &checkpoint;
        }
    }
    return nullptr;
}

/// Collect current snapshot journal checkpoints for all selected volumes.
/// If any volume is unavailable, returns empty list (baseline not usable for next Incremental).
[[nodiscard]] base::Result<std::vector<contracts::FileJournalCheckpoint>>
collect_current_checkpoints(ports::IFileSnapshotView& snapshot,
                            const std::vector<std::string>& volumes,
                            const base::CancellationToken& cancellation) {
    std::vector<contracts::FileJournalCheckpoint> checkpoints;
    checkpoints.reserve(volumes.size());
    for (const auto& volume_identity : volumes) {
        if (cancellation.stop_requested()) {
            return base::Result<std::vector<contracts::FileJournalCheckpoint>>::failure(
                err(base::ErrorCode::kCancelled, "file backup journal query cancelled"));
        }
        auto state = snapshot.query_journal_state(volume_identity, cancellation);
        if (!state) {
            return base::Result<std::vector<contracts::FileJournalCheckpoint>>::failure(
                state.error());
        }
        if (!state.value().available) {
            return base::Result<std::vector<contracts::FileJournalCheckpoint>>::success({});
        }
        contracts::FileJournalCheckpoint checkpoint;
        checkpoint.volume_identity = volume_identity;
        checkpoint.journal_id = state.value().journal_id;
        checkpoint.next_usn = state.value().next_usn;
        checkpoints.push_back(std::move(checkpoint));
    }
    return base::Result<std::vector<contracts::FileJournalCheckpoint>>::success(
        std::move(checkpoints));
}

[[nodiscard]] base::Result<std::vector<contracts::FileChangeHint>>
read_volume_change_hints(ports::IFileSnapshotView& snapshot, const std::string& volume_identity,
                         const std::int64_t start_usn, const std::int64_t end_usn,
                         const base::CancellationToken& cancellation) {
    std::vector<contracts::FileChangeHint> hints;
    std::int64_t cursor = start_usn;
    while (cursor < end_usn) {
        if (cancellation.stop_requested()) {
            return base::Result<std::vector<contracts::FileChangeHint>>::failure(
                err(base::ErrorCode::kCancelled, "file backup journal read cancelled"));
        }
        auto batch = snapshot.read_change_batch(volume_identity, cursor, end_usn,
                                                contracts::kMaximumChangeHintsPerBatch, cancellation);
        if (!batch) {
            return base::Result<std::vector<contracts::FileChangeHint>>::failure(batch.error());
        }
        for (auto& hint : batch.value().hints) {
            hints.push_back(std::move(hint));
        }
        if (!batch.value().next_start_usn.has_value()) {
            break;
        }
        if (batch.value().next_start_usn.value() <= cursor) {
            return base::Result<std::vector<contracts::FileChangeHint>>::failure(
                err(base::ErrorCode::kConflict, "file_backup.journal_wrapped"));
        }
        cursor = batch.value().next_start_usn.value();
    }
    return base::Result<std::vector<contracts::FileChangeHint>>::success(std::move(hints));
}

/// Defense-in-depth continuity check + USN range read for Incremental.
[[nodiscard]] base::Result<std::vector<contracts::FileChangeHint>>
collect_incremental_hints(ports::IFileSnapshotView& snapshot, const FileSetBackupPlan& plan,
                          const base::CancellationToken& cancellation) {
    if (!plan.change_hints.empty()) {
        return base::Result<std::vector<contracts::FileChangeHint>>::success(plan.change_hints);
    }
    std::vector<contracts::FileChangeHint> all_hints;
    const auto volumes = unique_volume_identities(plan.selections);
    for (const auto& volume_identity : volumes) {
        const auto* parent_cp = find_checkpoint(plan.parent_checkpoints, volume_identity);
        if (parent_cp == nullptr) {
            return base::Result<std::vector<contracts::FileChangeHint>>::failure(
                err(base::ErrorCode::kConflict, "file_backup.journal_missing"));
        }
        auto state = snapshot.query_journal_state(volume_identity, cancellation);
        if (!state) {
            return base::Result<std::vector<contracts::FileChangeHint>>::failure(state.error());
        }
        if (!state.value().available) {
            return base::Result<std::vector<contracts::FileChangeHint>>::failure(
                err(base::ErrorCode::kConflict, "file_backup.journal_inaccessible"));
        }
        if (state.value().journal_id != parent_cp->journal_id) {
            return base::Result<std::vector<contracts::FileChangeHint>>::failure(
                err(base::ErrorCode::kConflict, "file_backup.journal_reset"));
        }
        if (state.value().lowest_valid_usn > parent_cp->next_usn ||
            parent_cp->next_usn > state.value().next_usn) {
            return base::Result<std::vector<contracts::FileChangeHint>>::failure(
                err(base::ErrorCode::kConflict, "file_backup.journal_wrapped"));
        }
        auto volume_hints = read_volume_change_hints(snapshot, volume_identity, parent_cp->next_usn,
                                                     state.value().next_usn, cancellation);
        if (!volume_hints) {
            return base::Result<std::vector<contracts::FileChangeHint>>::failure(
                volume_hints.error());
        }
        all_hints.insert(all_hints.end(), std::make_move_iterator(volume_hints.value().begin()),
                         std::make_move_iterator(volume_hints.value().end()));
    }
    return base::Result<std::vector<contracts::FileChangeHint>>::success(std::move(all_hints));
}

struct StreamWriteContext final {
    ports::IFileSnapshotView& snapshot;
    ports::IFileBackupSession& session;
    const FileSetBackupPlan& plan;
    FileSetBackupSummary& summary;
    std::uint64_t& next_chunk_index;
    ports::IProgressSink* progress{nullptr};
};

[[nodiscard]] base::Result<void>
write_stream_content(StreamWriteContext& context, contracts::FileEntryDesc& entry,
                     contracts::FileStreamDesc& stream,
                     const base::CancellationToken& cancellation) {
    auto reader =
        context.snapshot.open_stream_reader(entry.entry_id, stream.stream_index, cancellation);
    if (!reader) {
        return base::Result<void>::failure(reader.error());
    }
    if (reader.value()->size_bytes() != stream.logical_size) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kConflict, "file stream size changed during backup"));
    }
    std::vector<std::byte> buffer(context.plan.chunk_size_bytes);
    std::uint64_t offset = 0;
    std::uint64_t logical_block = 0;
    while (offset < stream.logical_size) {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kCancelled, "file backup stream read cancelled"));
        }
        const auto remaining = stream.logical_size - offset;
        const auto want = static_cast<std::size_t>(
            (std::min)(remaining, static_cast<std::uint64_t>(context.plan.block_size_bytes)));
        auto read = reader.value()->read(offset, std::span(buffer).first(want), cancellation);
        if (!read) {
            return base::Result<void>::failure(read.error());
        }
        if (read.value() != want) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kIoFailure, "file stream short read before end"));
        }
        ports::FileChunkWriteRequest request;
        request.chunk_index = context.next_chunk_index;
        request.stream_index = stream.stream_index;
        request.logical_block_index = logical_block;
        request.logical_size = static_cast<std::uint32_t>(want);
        // Logical payload only; session applies opportunistic zstd (V7 COMPRESSION_ZSTD).
        request.payload = std::span<const std::byte>(buffer.data(), want);
        auto written = context.session.write_stream_chunk(request, cancellation);
        if (!written) {
            return base::Result<void>::failure(written.error());
        }
        contracts::FileStreamExtentDesc extent;
        extent.chunk_index = context.next_chunk_index;
        extent.block_entry_index = 0;
        extent.file_offset = offset;
        extent.logical_size = want;
        stream.extents.push_back(extent);
        ++context.next_chunk_index;
        ++logical_block;
        offset += want;
        auto processed = checked_add(context.summary.processed_bytes, want);
        if (!processed) {
            return base::Result<void>::failure(processed.error());
        }
        context.summary.processed_bytes = processed.value();
        auto stored = checked_add(context.summary.stored_bytes, written.value());
        if (!stored) {
            return base::Result<void>::failure(stored.error());
        }
        context.summary.stored_bytes = stored.value();
        ++context.summary.chunk_count;
        // Publish after each stream quantum so Desktop can poll live percent while writing.
        publish(context.progress, context.plan, contracts::TaskPhase::kWriting, context.summary,
                "file_backup.writing");
    }
    return base::Result<void>::success();
}

} // namespace

FileSetBackupPipeline::FileSetBackupPipeline(ports::IFileSnapshotView& snapshot,
                                             ports::IFileBackupSession& session,
                                             ports::IProgressSink* progress) noexcept
    : snapshot_(snapshot), session_(session), progress_(progress) {}

base::Result<FileSetBackupSummary>
FileSetBackupPipeline::run(const FileSetBackupPlan& plan,
                           const base::CancellationToken& cancellation) {
    auto validation = validate_plan(plan);
    if (!validation) {
        return base::Result<FileSetBackupSummary>::failure(validation.error());
    }
    SessionGuard guard(session_);
    FileSetBackupSummary summary;
    publish(progress_, plan, contracts::TaskPhase::kPreparing, summary, "file_backup.preparing");

    const auto volumes = unique_volume_identities(plan.selections);
    auto current_checkpoints = collect_current_checkpoints(snapshot_, volumes, cancellation);
    if (!current_checkpoints) {
        return base::Result<FileSetBackupSummary>::failure(current_checkpoints.error());
    }
    summary.journal_checkpoints = std::move(current_checkpoints).value();

    std::vector<contracts::FileChangeHint> hints;
    if (plan.effective_type == contracts::BackupType::kIncremental) {
        auto collected = collect_incremental_hints(snapshot_, plan, cancellation);
        if (!collected) {
            return base::Result<FileSetBackupSummary>::failure(collected.error());
        }
        hints = std::move(collected).value();
    }

    auto entries = enumerate_all(snapshot_, plan, cancellation, summary, progress_);
    if (!entries) {
        return base::Result<FileSetBackupSummary>::failure(entries.error());
    }

    FileSetChangePlannerRequest planner_request;
    planner_request.effective_type = plan.effective_type;
    planner_request.parent_reader = plan.parent_reader;
    planner_request.change_hints = std::move(hints);
    planner_request.parent_index_budget_bytes =
        plan.memory_budget_bytes > plan.chunk_size_bytes
            ? plan.memory_budget_bytes - plan.chunk_size_bytes
            : plan.memory_budget_bytes;

    auto planned =
        plan_file_set_streams(std::move(entries).value(), planner_request, cancellation);
    if (!planned) {
        return base::Result<FileSetBackupSummary>::failure(planned.error());
    }
    summary.stream_count = planned.value().stream_count;
    summary.logical_bytes = planned.value().logical_bytes;
    // Denominator for live percent = local payload only (matches write_stream_content loop).
    std::uint64_t local_logical = 0;
    for (const auto& item : planned.value().local_streams) {
        const auto& entry = planned.value().entries[item.entry_pos];
        if (item.stream_pos >= entry.streams.size()) {
            return base::Result<FileSetBackupSummary>::failure(
                err(base::ErrorCode::kInternal, "file backup local stream plan is inconsistent"));
        }
        auto next = checked_add(local_logical, entry.streams[item.stream_pos].logical_size);
        if (!next) {
            return base::Result<FileSetBackupSummary>::failure(next.error());
        }
        local_logical = next.value();
    }
    summary.progress_logical_bytes = local_logical;

    publish(progress_, plan, contracts::TaskPhase::kReading, summary, "file_backup.reading");
    std::uint64_t next_chunk_index = 0;
    StreamWriteContext write_context{snapshot_, session_, plan, summary, next_chunk_index,
                                     progress_};
    for (const auto& item : planned.value().local_streams) {
        auto& entry = planned.value().entries[item.entry_pos];
        auto& stream = entry.streams[item.stream_pos];
        auto written = write_stream_content(write_context, entry, stream, cancellation);
        if (!written) {
            return base::Result<FileSetBackupSummary>::failure(written.error());
        }
    }
    publish(progress_, plan, contracts::TaskPhase::kWriting, summary, "file_backup.indexing");
    for (const auto& entry : planned.value().entries) {
        if (cancellation.stop_requested()) {
            return base::Result<FileSetBackupSummary>::failure(
                err(base::ErrorCode::kCancelled, "file backup cancelled"));
        }
        auto written = session_.write_entry(entry, cancellation);
        if (!written) {
            return base::Result<FileSetBackupSummary>::failure(written.error());
        }
        auto next = checked_add(summary.processed_entries, 1);
        if (!next) {
            return base::Result<FileSetBackupSummary>::failure(next.error());
        }
        summary.processed_entries = next.value();
        // Keep last_progress fresh while indexing (bytes already complete; entries advance).
        publish(progress_, plan, contracts::TaskPhase::kWriting, summary, "file_backup.indexing");
    }
    publish(progress_, plan, contracts::TaskPhase::kCommitting, summary, "file_backup.finalizing");
    auto finalized = session_.finalize(cancellation);
    if (!finalized) {
        return base::Result<FileSetBackupSummary>::failure(finalized.error());
    }
    auto committed = session_.commit(cancellation);
    if (!committed) {
        return base::Result<FileSetBackupSummary>::failure(committed.error());
    }
    guard.mark_committed();
    publish(progress_, plan, contracts::TaskPhase::kCompleted, summary, "file_backup.completed");
    return base::Result<FileSetBackupSummary>::success(std::move(summary));
}

} // namespace aegra::pipeline
