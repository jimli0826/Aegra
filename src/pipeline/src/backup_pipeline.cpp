#include "aegra/pipeline/backup_pipeline.h"

#include "aegra/pipeline/fixed_size_chunker.h"
#include "bounded_chunk_queue.h"

#include <limits>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace aegra::pipeline {
namespace {

class SessionGuard final {
  public:
    explicit SessionGuard(ports::IBackupSession& session) noexcept : session_(session) {}
    ~SessionGuard() {
        if (!committed_) {
            session_.abort();
        }
    }

    void mark_committed() noexcept { committed_ = true; }

    SessionGuard(const SessionGuard&) = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;
    SessionGuard(SessionGuard&&) = delete;
    SessionGuard& operator=(SessionGuard&&) = delete;

  private:
    ports::IBackupSession& session_;
    bool committed_{false};
};

struct BackupProducerContext final {
    ports::IBlockSource& source;
    const FixedSizeChunker& chunker;
    detail::BoundedChunkQueue& queue;
    base::CancellationToken cancellation;
};

struct BackupConsumerContext final {
    const BackupPlan& plan;
    ports::IBackupSession& session;
    ports::IProgressSink* progress;
    detail::BoundedChunkQueue& queue;
    base::CancellationToken cancellation;
    std::uint64_t logical_size;
};

base::Result<void> validate_plan(const BackupPlan& plan) {
    if (plan.job_id.empty() || plan.chunk_size_bytes == 0 || plan.memory_budget_bytes == 0) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "backup plan is incomplete"});
    }
    if (plan.chunk_size_bytes > plan.memory_budget_bytes ||
        plan.chunk_size_bytes > (std::numeric_limits<std::size_t>::max)()) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInsufficientSpace,
            "backup chunk size exceeds memory budget",
        });
    }
    return base::Result<void>::success();
}

base::Result<std::vector<std::byte>> read_range(ports::IBlockSource& source,
                                                const ChunkRange& range,
                                                const base::CancellationToken& cancellation) {
    std::vector<std::byte> payload(static_cast<std::size_t>(range.logical_size));
    std::size_t filled = 0;
    while (filled < payload.size()) {
        auto destination = std::span<std::byte>(payload).subspan(filled);
        auto result = source.read(range.logical_offset + filled, destination, cancellation);
        if (!result) {
            return base::Result<std::vector<std::byte>>::failure(result.error());
        }
        if (result.value() == 0) {
            return base::Result<std::vector<std::byte>>::failure(base::Error{
                base::ErrorCode::kIoFailure,
                "block source returned zero bytes before end of range",
            });
        }
        filled += result.value();
    }
    return base::Result<std::vector<std::byte>>::success(std::move(payload));
}

base::Result<ports::ChunkData> read_next_chunk(BackupProducerContext& context,
                                               const ChunkRange& range) {
    auto payload = read_range(context.source, range, context.cancellation);
    if (!payload) {
        return base::Result<ports::ChunkData>::failure(payload.error());
    }
    ports::ChunkDescriptor descriptor{
        range.chunk_index,
        range.logical_offset,
        range.logical_size,
        static_cast<std::uint64_t>(payload.value().size()),
    };
    return base::Result<ports::ChunkData>::success(
        ports::ChunkData{descriptor, std::move(payload).value()});
}

base::Result<void> produce_all_chunks(BackupProducerContext& context) {
    const auto total_size = context.source.size_bytes();
    std::uint64_t offset = 0;
    std::uint64_t index = 0;
    while (offset < total_size) {
        auto range_result = context.chunker.next(total_size, offset, index);
        if (!range_result) {
            return base::Result<void>::failure(range_result.error());
        }
        auto range_optional = std::move(range_result).value();
        if (!range_optional.has_value()) {
            return base::Result<void>::failure(
                base::Error{base::ErrorCode::kInternal, "chunker ended before source end"});
        }
        const auto range = range_optional.value();
        auto chunk = read_next_chunk(context, range);
        if (!chunk) {
            return base::Result<void>::failure(chunk.error());
        }
        auto pushed = context.queue.push(std::move(chunk).value(), context.cancellation);
        if (!pushed) {
            return pushed;
        }
        offset += range.logical_size;
        ++index;
    }
    return base::Result<void>::success();
}

void produce_chunks(BackupProducerContext& context) {
    try {
        auto result = produce_all_chunks(context);
        if (!result) {
            context.queue.fail(result.error());
        } else {
            context.queue.close();
        }
    } catch (...) {
        context.queue.fail(
            base::Error{base::ErrorCode::kInternal, "backup producer failed unexpectedly"});
    }
}

void publish_progress(ports::IProgressSink* sink, const BackupPlan& plan,
                      const contracts::TaskPhase phase, const BackupSummary& summary) {
    if (sink == nullptr) {
        return;
    }
    sink->publish(contracts::TaskProgress{
        contracts::kTaskProgressSchemaVersion,
        plan.job_id,
        phase,
        summary.logical_bytes,
        summary.stored_bytes,
        summary.stored_bytes,
        {},
    });
}

base::Result<BackupSummary> consume_chunks(BackupConsumerContext& context) {
    BackupSummary summary;
    summary.logical_bytes = context.logical_size;
    SessionGuard guard(context.session);
    while (true) {
        auto next = context.queue.pop(context.cancellation);
        if (!next) {
            return base::Result<BackupSummary>::failure(next.error());
        }
        auto chunk_optional = std::move(next).value();
        if (!chunk_optional.has_value()) {
            break;
        }
        auto chunk = std::move(chunk_optional).value();
        auto written = context.session.write_chunk(
            ports::ChunkWriteRequest{chunk.descriptor, chunk.payload}, context.cancellation);
        if (!written) {
            return base::Result<BackupSummary>::failure(written.error());
        }
        summary.stored_bytes += chunk.descriptor.stored_size;
        ++summary.chunk_count;
        publish_progress(context.progress, context.plan, contracts::TaskPhase::kWriting, summary);
    }
    publish_progress(context.progress, context.plan, contracts::TaskPhase::kCommitting, summary);
    auto committed = context.session.commit(context.cancellation);
    if (!committed) {
        return base::Result<BackupSummary>::failure(committed.error());
    }
    guard.mark_committed();
    summary.peak_buffered_bytes = context.queue.peak_buffered_bytes();
    publish_progress(context.progress, context.plan, contracts::TaskPhase::kCompleted, summary);
    return base::Result<BackupSummary>::success(summary);
}

base::Result<BackupSummary> consume_safely(BackupConsumerContext& context,
                                           std::stop_source& local_stop) {
    try {
        auto result = consume_chunks(context);
        if (!result) {
            local_stop.request_stop();
        }
        return result;
    } catch (...) {
        local_stop.request_stop();
        return base::Result<BackupSummary>::failure(
            base::Error{base::ErrorCode::kInternal, "backup consumer failed unexpectedly"});
    }
}

} // namespace

BackupPipeline::BackupPipeline(ports::IBlockSource& source, ports::IBackupSession& session,
                               ports::IProgressSink* progress) noexcept
    : source_(source), session_(session), progress_(progress) {}

base::Result<BackupSummary> BackupPipeline::run(const BackupPlan& plan,
                                                const base::CancellationToken& cancellation) {
    const auto validation = validate_plan(plan);
    if (!validation) {
        session_.abort();
        return base::Result<BackupSummary>::failure(validation.error());
    }
    auto chunker_result = FixedSizeChunker::create(plan.chunk_size_bytes);
    if (!chunker_result) {
        session_.abort();
        return base::Result<BackupSummary>::failure(chunker_result.error());
    }

    detail::BoundedChunkQueue queue(plan.memory_budget_bytes);
    std::stop_source local_stop;
    std::stop_callback external_cancel(cancellation, [&local_stop] { local_stop.request_stop(); });
    BackupProducerContext producer_context{source_, chunker_result.value(), queue,
                                           local_stop.get_token()};
    BackupConsumerContext consumer_context{
        plan, session_, progress_, queue, local_stop.get_token(), source_.size_bytes()};
    std::jthread producer([&producer_context] { produce_chunks(producer_context); });

    return consume_safely(consumer_context, local_stop);
}

} // namespace aegra::pipeline
