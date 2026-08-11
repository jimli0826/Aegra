#include "aegra/pipeline/backup_pipeline.h"

#include "aegra/pipeline/fixed_size_chunker.h"
#include "bounded_chunk_queue.h"
#include "chunk_buffer_pool.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aegra::pipeline {
namespace {

struct BackupPipelineTimings final {
    std::atomic<std::uint64_t> producer_read{0};
    std::atomic<std::uint64_t> producer_payload_allocate{0};
    std::atomic<std::uint64_t> producer_buffer_wait{0};
    std::atomic<std::uint64_t> producer_extent_describe{0};
    std::atomic<std::uint64_t> producer_source_read{0};
    std::atomic<std::uint64_t> producer_source_read_bytes{0};
    std::atomic<std::uint64_t> producer_free_bytes{0};
    std::atomic<std::uint64_t> producer_extent_describe_calls{0};
    std::atomic<std::uint64_t> producer_source_read_calls{0};
    std::atomic<std::uint64_t> producer_queue_wait{0};
    std::atomic<std::uint64_t> consumer_queue_wait{0};
    std::atomic<std::uint64_t> consumer_write{0};
    std::atomic<std::uint64_t> consumer_progress{0};
};

[[nodiscard]] std::uint64_t
elapsed_microseconds(const std::chrono::steady_clock::time_point start) noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
}

// Service / worker wire integers are signed 64-bit; progress must stay in that range.
constexpr std::uint64_t kMaximumWireInteger =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

[[nodiscard]] base::Result<std::uint64_t> checked_add_wire(const std::uint64_t left,
                                                           const std::uint64_t right) {
    if (left > kMaximumWireInteger || right > kMaximumWireInteger) {
        return base::Result<std::uint64_t>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "progress byte count exceeds the signed 64-bit wire range",
        });
    }
    if (right > kMaximumWireInteger - left) {
        return base::Result<std::uint64_t>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "progress byte accumulation overflow",
        });
    }
    return base::Result<std::uint64_t>::success(left + right);
}

[[nodiscard]] base::Result<void> require_wire_range(const std::uint64_t value, const char* field) {
    if (value > kMaximumWireInteger) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            std::string(field) + " exceeds the signed 64-bit wire range",
        });
    }
    return base::Result<void>::success();
}

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
    detail::ChunkBufferPool& buffer_pool;
    base::CancellationToken cancellation;
    std::uint32_t source_index{0};
    BackupPipelineTimings& timings;
};

struct BackupConsumerContext final {
    const BackupPlan& plan;
    ports::IBackupSession& session;
    ports::IProgressSink* progress;
    detail::BoundedChunkQueue& queue;
    detail::ChunkBufferPool& buffer_pool;
    base::CancellationToken cancellation;
    std::uint64_t logical_size;
    BackupPipelineTimings& timings;
};

struct ReadRangeResult final {
    detail::OwnedChunkBuffer payload;
    std::vector<ports::ChunkFreeRange> free_ranges;
};

base::Result<void> validate_plan(const BackupPlan& plan) {
    if (plan.job_id.empty() || plan.trace_id.empty() || plan.chunk_size_bytes == 0 ||
        plan.memory_budget_bytes == 0) {
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
    auto total_ok = require_wire_range(plan.progress_total_logical_bytes, "progress total logical");
    if (!total_ok) {
        return total_ok;
    }
    auto base_processed_ok =
        require_wire_range(plan.progress_base_processed_bytes, "progress base processed");
    if (!base_processed_ok) {
        return base_processed_ok;
    }
    auto base_stored_ok =
        require_wire_range(plan.progress_base_stored_bytes, "progress base stored");
    if (!base_stored_ok) {
        return base_stored_ok;
    }
    if (plan.progress_total_logical_bytes != 0 &&
        plan.progress_base_processed_bytes > plan.progress_total_logical_bytes) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "progress base processed exceeds job total logical bytes",
        });
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_progress_window(const BackupPlan& plan,
                                                          const std::uint64_t source_size) {
    auto source_ok = require_wire_range(source_size, "volume logical size");
    if (!source_ok) {
        return source_ok;
    }
    if (plan.progress_total_logical_bytes == 0) {
        return base::Result<void>::success();
    }
    auto volume_end = checked_add_wire(plan.progress_base_processed_bytes, source_size);
    if (!volume_end) {
        return base::Result<void>::failure(volume_end.error());
    }
    if (volume_end.value() > plan.progress_total_logical_bytes) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "volume progress window exceeds job total logical bytes",
        });
    }
    return base::Result<void>::success();
}

base::Result<void> read_data_extent(ports::IBlockSource& source, const std::uint64_t offset,
                                    std::span<std::byte> destination,
                                    const base::CancellationToken& cancellation,
                                    BackupPipelineTimings& timings) {
    std::size_t filled = 0;
    while (filled < destination.size()) {
        const auto read_start = std::chrono::steady_clock::now();
        auto result = source.read(offset + filled, destination.subspan(filled), cancellation);
        timings.producer_source_read.fetch_add(elapsed_microseconds(read_start),
                                               std::memory_order_relaxed);
        timings.producer_source_read_calls.fetch_add(1, std::memory_order_relaxed);
        if (!result) {
            return base::Result<void>::failure(result.error());
        }
        if (result.value() == 0) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kIoFailure,
                "block source returned zero bytes before end of range",
            });
        }
        timings.producer_source_read_bytes.fetch_add(result.value(), std::memory_order_relaxed);
        filled += result.value();
    }
    return base::Result<void>::success();
}

base::Result<ReadRangeResult> read_range(ports::IBlockSource& source, const ChunkRange& range,
                                         const base::CancellationToken& cancellation,
                                         BackupPipelineTimings& timings,
                                         detail::OwnedChunkBuffer payload) {
    ReadRangeResult result;
    result.payload = std::move(payload);
    std::uint64_t consumed = 0;
    while (consumed < range.logical_size) {
        const auto remaining = range.logical_size - consumed;
        const auto describe_start = std::chrono::steady_clock::now();
        auto extent =
            source.describe_extent(range.logical_offset + consumed, remaining, cancellation);
        timings.producer_extent_describe.fetch_add(elapsed_microseconds(describe_start),
                                                   std::memory_order_relaxed);
        timings.producer_extent_describe_calls.fetch_add(1, std::memory_order_relaxed);
        if (!extent) {
            return base::Result<ReadRangeResult>::failure(extent.error());
        }
        if (extent.value().logical_offset != range.logical_offset + consumed ||
            extent.value().logical_size == 0 || extent.value().logical_size > remaining) {
            return base::Result<ReadRangeResult>::failure(
                base::Error{base::ErrorCode::kInternal, "block source returned an invalid extent"});
        }
        if (extent.value().state == ports::BlockExtentState::kFree) {
            result.free_ranges.push_back({consumed, extent.value().logical_size});
            timings.producer_free_bytes.fetch_add(extent.value().logical_size,
                                                  std::memory_order_relaxed);
        } else if (extent.value().state == ports::BlockExtentState::kData) {
            const auto output = result.payload.bytes().subspan(
                static_cast<std::size_t>(consumed),
                static_cast<std::size_t>(extent.value().logical_size));
            auto read = read_data_extent(source, extent.value().logical_offset, output,
                                         cancellation, timings);
            if (!read) {
                return base::Result<ReadRangeResult>::failure(read.error());
            }
        } else {
            return base::Result<ReadRangeResult>::failure(
                {base::ErrorCode::kInternal, "block source returned an unknown extent state"});
        }
        consumed += extent.value().logical_size;
    }
    return base::Result<ReadRangeResult>::success(std::move(result));
}

base::Result<detail::QueuedChunk> read_next_chunk(BackupProducerContext& context,
                                                  const ChunkRange& range) {
    auto buffer = context.buffer_pool.acquire(static_cast<std::size_t>(range.logical_size),
                                              context.cancellation);
    if (!buffer) {
        return base::Result<detail::QueuedChunk>::failure(buffer.error());
    }
    context.timings.producer_buffer_wait.fetch_add(buffer.value().wait_microseconds,
                                                   std::memory_order_relaxed);
    context.timings.producer_payload_allocate.fetch_add(buffer.value().allocate_microseconds,
                                                        std::memory_order_relaxed);
    auto payload = read_range(context.source, range, context.cancellation, context.timings,
                              std::move(buffer.value().buffer));
    if (!payload) {
        return base::Result<detail::QueuedChunk>::failure(payload.error());
    }
    ports::ChunkDescriptor descriptor{
        range.chunk_index,    range.logical_offset,
        range.logical_size,   static_cast<std::uint64_t>(payload.value().payload.bytes().size()),
        context.source_index, std::move(payload.value().free_ranges),
    };
    return base::Result<detail::QueuedChunk>::success(
        detail::QueuedChunk{std::move(descriptor), std::move(payload.value().payload)});
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
        const auto read_start = std::chrono::steady_clock::now();
        auto chunk = read_next_chunk(context, range);
        context.timings.producer_read.fetch_add(elapsed_microseconds(read_start),
                                                std::memory_order_relaxed);
        if (!chunk) {
            return base::Result<void>::failure(chunk.error());
        }
        const auto queue_start = std::chrono::steady_clock::now();
        auto pushed = context.queue.push(std::move(chunk).value(), context.cancellation);
        context.timings.producer_queue_wait.fetch_add(elapsed_microseconds(queue_start),
                                                      std::memory_order_relaxed);
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

[[nodiscard]] const char* backup_phase_message(const contracts::TaskPhase phase) noexcept {
    switch (phase) {
    case contracts::TaskPhase::kPreparing:
        return "backup.preparing";
    case contracts::TaskPhase::kReading:
        return "backup.reading";
    case contracts::TaskPhase::kTransforming:
        return "backup.transforming";
    case contracts::TaskPhase::kWriting:
        return "backup.writing";
    case contracts::TaskPhase::kCommitting:
        return "backup.committing";
    case contracts::TaskPhase::kCompleted:
        return "backup.completed";
    case contracts::TaskPhase::kUnspecified:
        break;
    }
    return "backup.running";
}

base::Result<void> publish_progress(ports::IProgressSink* sink, const BackupPlan& plan,
                                    const contracts::TaskPhase phase, const BackupSummary& summary,
                                    const std::uint64_t volume_processed_bytes) {
    if (sink == nullptr) {
        return base::Result<void>::success();
    }
    auto processed = checked_add_wire(plan.progress_base_processed_bytes, volume_processed_bytes);
    if (!processed) {
        return base::Result<void>::failure(processed.error());
    }
    auto stored = checked_add_wire(plan.progress_base_stored_bytes, summary.stored_bytes);
    if (!stored) {
        return base::Result<void>::failure(stored.error());
    }
    std::uint64_t logical = summary.logical_bytes;
    if (plan.progress_total_logical_bytes != 0) {
        logical = plan.progress_total_logical_bytes;
        if (processed.value() > logical) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "aggregated processed_bytes exceeds job total logical bytes",
            });
        }
    } else {
        auto logical_ok = require_wire_range(logical, "volume logical bytes");
        if (!logical_ok) {
            return logical_ok;
        }
    }
    auto stored_ok = require_wire_range(stored.value(), "stored bytes");
    if (!stored_ok) {
        return stored_ok;
    }
    sink->publish(contracts::make_byte_progress(plan.job_id, plan.trace_id, phase, logical,
                                                processed.value(), stored.value(),
                                                backup_phase_message(phase)));
    return base::Result<void>::success();
}

void copy_timing_summary(BackupSummary& summary, const BackupPipelineTimings& timings) noexcept {
    summary.producer_read_microseconds = timings.producer_read.load(std::memory_order_relaxed);
    summary.producer_payload_allocate_microseconds =
        timings.producer_payload_allocate.load(std::memory_order_relaxed);
    summary.producer_buffer_wait_microseconds =
        timings.producer_buffer_wait.load(std::memory_order_relaxed);
    summary.producer_extent_describe_microseconds =
        timings.producer_extent_describe.load(std::memory_order_relaxed);
    summary.producer_source_read_microseconds =
        timings.producer_source_read.load(std::memory_order_relaxed);
    summary.producer_source_read_bytes =
        timings.producer_source_read_bytes.load(std::memory_order_relaxed);
    summary.producer_free_bytes = timings.producer_free_bytes.load(std::memory_order_relaxed);
    summary.producer_extent_describe_calls =
        timings.producer_extent_describe_calls.load(std::memory_order_relaxed);
    summary.producer_source_read_calls =
        timings.producer_source_read_calls.load(std::memory_order_relaxed);
    summary.producer_queue_wait_microseconds =
        timings.producer_queue_wait.load(std::memory_order_relaxed);
    summary.consumer_queue_wait_microseconds =
        timings.consumer_queue_wait.load(std::memory_order_relaxed);
    summary.consumer_write_microseconds = timings.consumer_write.load(std::memory_order_relaxed);
    summary.consumer_progress_microseconds =
        timings.consumer_progress.load(std::memory_order_relaxed);
}

base::Result<BackupSummary> consume_chunks(BackupConsumerContext& context) {
    BackupSummary summary;
    summary.logical_bytes = context.logical_size;
    std::uint64_t processed_bytes = 0;
    SessionGuard guard(context.session);
    while (true) {
        const auto queue_start = std::chrono::steady_clock::now();
        auto next = context.queue.pop(context.cancellation);
        context.timings.consumer_queue_wait.fetch_add(elapsed_microseconds(queue_start),
                                                      std::memory_order_relaxed);
        if (!next) {
            return base::Result<BackupSummary>::failure(next.error());
        }
        auto chunk_optional = std::move(next).value();
        if (!chunk_optional.has_value()) {
            break;
        }
        auto chunk = std::move(chunk_optional).value();
        const auto write_start = std::chrono::steady_clock::now();
        auto written = context.session.write_chunk(
            ports::ChunkWriteRequest{chunk.descriptor, chunk.payload.bytes()},
            context.cancellation);
        context.timings.consumer_write.fetch_add(elapsed_microseconds(write_start),
                                                 std::memory_order_relaxed);
        context.buffer_pool.release(std::move(chunk.payload));
        if (!written) {
            return base::Result<BackupSummary>::failure(written.error());
        }
        auto next_processed = checked_add_wire(processed_bytes, chunk.descriptor.logical_size);
        if (!next_processed) {
            return base::Result<BackupSummary>::failure(next_processed.error());
        }
        processed_bytes = next_processed.value();
        auto next_stored = checked_add_wire(summary.stored_bytes, chunk.descriptor.stored_size);
        if (!next_stored) {
            return base::Result<BackupSummary>::failure(next_stored.error());
        }
        summary.stored_bytes = next_stored.value();
        ++summary.chunk_count;
        const auto progress_start = std::chrono::steady_clock::now();
        auto published = publish_progress(context.progress, context.plan,
                                          contracts::TaskPhase::kWriting, summary, processed_bytes);
        context.timings.consumer_progress.fetch_add(elapsed_microseconds(progress_start),
                                                    std::memory_order_relaxed);
        if (!published) {
            return base::Result<BackupSummary>::failure(published.error());
        }
    }
    summary.peak_buffered_bytes = context.queue.peak_buffered_bytes();
    copy_timing_summary(summary, context.timings);
    // Only the final volume (kCommit) commits the archive and publishes kCompleted.
    // Intermediate volumes use kDefer: keep session open and stay in kWriting.
    if (context.plan.commit_mode == BackupCommitMode::kCommit) {
        auto committing =
            publish_progress(context.progress, context.plan, contracts::TaskPhase::kCommitting,
                             summary, processed_bytes);
        if (!committing) {
            return base::Result<BackupSummary>::failure(committing.error());
        }
        auto committed = context.session.commit(context.cancellation);
        if (!committed) {
            return base::Result<BackupSummary>::failure(committed.error());
        }
        guard.mark_committed();
        auto completed =
            publish_progress(context.progress, context.plan, contracts::TaskPhase::kCompleted,
                             summary, processed_bytes);
        if (!completed) {
            return base::Result<BackupSummary>::failure(completed.error());
        }
    } else {
        guard.mark_committed(); // do not abort shared multi-volume session
        auto writing = publish_progress(context.progress, context.plan,
                                        contracts::TaskPhase::kWriting, summary, processed_bytes);
        if (!writing) {
            return base::Result<BackupSummary>::failure(writing.error());
        }
    }
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
    const auto source_size = source_.size_bytes();
    auto window = validate_progress_window(plan, source_size);
    if (!window) {
        session_.abort();
        return base::Result<BackupSummary>::failure(window.error());
    }
    auto chunker_result = FixedSizeChunker::create(plan.chunk_size_bytes);
    if (!chunker_result) {
        session_.abort();
        return base::Result<BackupSummary>::failure(chunker_result.error());
    }

    const auto maximum_buffers =
        plan.memory_budget_bytes / static_cast<std::size_t>(plan.chunk_size_bytes);
    detail::ChunkBufferPool buffer_pool(maximum_buffers);
    detail::BoundedChunkQueue queue(plan.memory_budget_bytes);
    std::stop_source local_stop;
    std::stop_callback external_cancel(cancellation, [&local_stop] { local_stop.request_stop(); });
    BackupPipelineTimings timings;
    BackupProducerContext producer_context{source_,     chunker_result.value(), queue,
                                           buffer_pool, local_stop.get_token(), plan.source_index,
                                           timings};
    BackupConsumerContext consumer_context{plan,        session_,    progress_,
                                           queue,       buffer_pool, local_stop.get_token(),
                                           source_size, timings};
    std::jthread producer([&producer_context] { produce_chunks(producer_context); });

    return consume_safely(consumer_context, local_stop);
}

} // namespace aegra::pipeline
