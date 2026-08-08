#include "aegra/pipeline/file_set_backup_pipeline.h"

#include "aegra/base/error.h"
#include "aegra/format/personal_archive.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
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
    progress.logical_bytes = summary.logical_bytes == 0 ? std::nullopt
                                                        : std::optional(summary.logical_bytes);
    progress.processed_bytes = summary.stored_bytes;
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

struct StreamWork final {
    std::uint64_t entry_id{0};
    std::uint32_t stream_index{0};
    std::size_t entry_pos{0};
    std::size_t stream_pos{0};
    bool materialize{true};
};

[[nodiscard]] base::Result<void>
plan_streams(std::vector<contracts::FileEntryDesc>& entries, std::vector<StreamWork>& work,
             FileSetBackupSummary& summary) {
    std::unordered_map<std::uint64_t, std::pair<std::size_t, std::size_t>> hard_link_source;
    std::uint32_t next_stream_index = 1;
    for (std::size_t entry_pos = 0; entry_pos < entries.size(); ++entry_pos) {
        auto& entry = entries[entry_pos];
        if (entry.entry_id == 0) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kInvalidArgument, "file entry id must be non-zero"));
        }
        if (entry.hard_link_group != 0) {
            const auto existing = hard_link_source.find(entry.hard_link_group);
            if (existing != hard_link_source.end()) {
                entry.streams = entries[existing->second.first].streams;
                continue;
            }
            hard_link_source.emplace(entry.hard_link_group, std::pair{entry_pos, 0});
        }
        for (std::size_t stream_pos = 0; stream_pos < entry.streams.size(); ++stream_pos) {
            auto& stream = entry.streams[stream_pos];
            if (stream.stream_index == 0) {
                stream.stream_index = next_stream_index++;
            } else {
                next_stream_index = (std::max)(next_stream_index, stream.stream_index + 1);
            }
            stream.extents.clear();
            if (stream.logical_size == 0) {
                continue;
            }
            StreamWork item;
            item.entry_id = entry.entry_id;
            item.stream_index = stream.stream_index;
            item.entry_pos = entry_pos;
            item.stream_pos = stream_pos;
            work.push_back(item);
            auto next = checked_add(summary.stream_count, 1);
            if (!next) {
                return base::Result<void>::failure(next.error());
            }
            summary.stream_count = next.value();
            auto bytes = checked_add(summary.logical_bytes, stream.logical_size);
            if (!bytes) {
                return base::Result<void>::failure(bytes.error());
            }
            summary.logical_bytes = bytes.value();
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
write_stream_content(ports::IFileSnapshotView& snapshot, ports::IFileBackupSession& session,
                     contracts::FileEntryDesc& entry, contracts::FileStreamDesc& stream,
                     const FileSetBackupPlan& plan, FileSetBackupSummary& summary,
                     std::uint64_t& next_chunk_index, const base::CancellationToken& cancellation) {
    auto reader = snapshot.open_stream_reader(entry.entry_id, stream.stream_index, cancellation);
    if (!reader) {
        return base::Result<void>::failure(reader.error());
    }
    if (reader.value()->size_bytes() != stream.logical_size) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kConflict, "file stream size changed during backup"));
    }
    std::vector<std::byte> buffer(plan.chunk_size_bytes);
    std::uint64_t offset = 0;
    std::uint64_t logical_block = 0;
    while (offset < stream.logical_size) {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kCancelled, "file backup stream read cancelled"));
        }
        const auto remaining = stream.logical_size - offset;
        const auto want = static_cast<std::size_t>(
            (std::min)(remaining, static_cast<std::uint64_t>(plan.block_size_bytes)));
        auto read = reader.value()->read(offset, std::span(buffer).first(want), cancellation);
        if (!read) {
            return base::Result<void>::failure(read.error());
        }
        if (read.value() != want) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kIoFailure, "file stream short read before end"));
        }
        ports::FileChunkWriteRequest request;
        request.chunk_index = next_chunk_index;
        request.stream_index = stream.stream_index;
        request.logical_block_index = logical_block;
        request.logical_size = static_cast<std::uint32_t>(want);
        request.block_flags = format::personal_archive::kBlockFlagRaw;
        request.payload = std::span<const std::byte>(buffer.data(), want);
        auto written = session.write_stream_chunk(request, cancellation);
        if (!written) {
            return written;
        }
        contracts::FileStreamExtentDesc extent;
        extent.chunk_index = next_chunk_index;
        extent.block_entry_index = 0;
        extent.file_offset = offset;
        extent.logical_size = want;
        stream.extents.push_back(extent);
        ++next_chunk_index;
        ++logical_block;
        offset += want;
        auto stored = checked_add(summary.stored_bytes, want);
        if (!stored) {
            return base::Result<void>::failure(stored.error());
        }
        summary.stored_bytes = stored.value();
        ++summary.chunk_count;
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
    auto entries = enumerate_all(snapshot_, plan, cancellation, summary, progress_);
    if (!entries) {
        return base::Result<FileSetBackupSummary>::failure(entries.error());
    }
    std::vector<StreamWork> work;
    auto planned = plan_streams(entries.value(), work, summary);
    if (!planned) {
        return base::Result<FileSetBackupSummary>::failure(planned.error());
    }
    publish(progress_, plan, contracts::TaskPhase::kReading, summary, "file_backup.reading");
    std::uint64_t next_chunk_index = 0;
    for (const auto& item : work) {
        auto& entry = entries.value()[item.entry_pos];
        auto& stream = entry.streams[item.stream_pos];
        auto written = write_stream_content(snapshot_, session_, entry, stream, plan, summary,
                                            next_chunk_index, cancellation);
        if (!written) {
            return base::Result<FileSetBackupSummary>::failure(written.error());
        }
    }
    publish(progress_, plan, contracts::TaskPhase::kWriting, summary, "file_backup.indexing");
    for (const auto& entry : entries.value()) {
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
