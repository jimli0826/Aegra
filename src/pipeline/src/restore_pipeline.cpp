#include "aegra/pipeline/restore_pipeline.h"

#include "bounded_chunk_queue.h"

#include <stop_token>
#include <thread>
#include <utility>

namespace aegra::pipeline {
namespace {

struct RestoreProducerContext final {
    ports::IRecoveryPointReader& reader;
    detail::BoundedChunkQueue& queue;
    base::CancellationToken cancellation;
};

struct RestoreConsumerContext final {
    const RestorePlan& plan;
    ports::IBlockSink& sink;
    ports::IProgressSink* progress;
    detail::BoundedChunkQueue& queue;
    base::CancellationToken cancellation;
    std::uint64_t logical_size;
};

base::Result<void> validate_plan(const RestorePlan& plan) {
    if (plan.job_id.empty() || plan.trace_id.empty() || plan.memory_budget_bytes == 0) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "restore plan is incomplete"});
    }
    return base::Result<void>::success();
}

base::Result<void> validate_descriptor(const ports::ChunkDescriptor& descriptor,
                                       const std::uint64_t expected_index,
                                       const std::uint64_t expected_offset,
                                       const std::uint64_t logical_size) {
    if (descriptor.chunk_index != expected_index || descriptor.logical_offset != expected_offset) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kCorruptData, "chunk mapping is not contiguous"});
    }
    if (descriptor.logical_size == 0 || descriptor.stored_size != descriptor.logical_size) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kCorruptData, "stage 2 requires non-empty raw chunks"});
    }
    if (expected_offset > logical_size ||
        descriptor.logical_size > logical_size - expected_offset) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kCorruptData, "chunk exceeds logical source size"});
    }
    return base::Result<void>::success();
}

base::Result<void> preflight_restore(const RestorePlan& plan, ports::IRecoveryPointReader& reader,
                                     ports::IBlockSink& sink) {
    const auto logical_size = reader.logical_size_bytes();
    if (logical_size > sink.capacity_bytes()) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInsufficientSpace,
            "restore target capacity is insufficient",
        });
    }

    std::uint64_t expected_offset = 0;
    for (std::uint64_t index = 0; index < reader.chunk_count(); ++index) {
        auto descriptor = reader.describe_chunk(index);
        if (!descriptor) {
            return base::Result<void>::failure(descriptor.error());
        }
        auto valid = validate_descriptor(descriptor.value(), index, expected_offset, logical_size);
        if (!valid) {
            return valid;
        }
        if (descriptor.value().stored_size > plan.memory_budget_bytes) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kInsufficientSpace,
                "stored chunk exceeds restore memory budget",
            });
        }
        expected_offset += descriptor.value().logical_size;
    }
    if (expected_offset != logical_size) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kCorruptData, "chunks do not cover logical source"});
    }
    return base::Result<void>::success();
}

base::Result<ports::ChunkData> read_validated_chunk(RestoreProducerContext& context,
                                                    const std::uint64_t index) {
    auto descriptor = context.reader.describe_chunk(index);
    if (!descriptor) {
        return base::Result<ports::ChunkData>::failure(descriptor.error());
    }
    auto chunk = context.reader.read_chunk(index, context.cancellation);
    if (!chunk) {
        return chunk;
    }
    if (chunk.value().descriptor != descriptor.value() ||
        chunk.value().payload.size() != descriptor.value().stored_size) {
        return base::Result<ports::ChunkData>::failure(
            base::Error{base::ErrorCode::kCorruptData, "chunk payload changed after preflight"});
    }
    return chunk;
}

base::Result<void> produce_all_chunks(RestoreProducerContext& context) {
    for (std::uint64_t index = 0; index < context.reader.chunk_count(); ++index) {
        auto chunk = read_validated_chunk(context, index);
        if (!chunk) {
            return base::Result<void>::failure(chunk.error());
        }
        auto pushed = context.queue.push(std::move(chunk).value(), context.cancellation);
        if (!pushed) {
            return pushed;
        }
    }
    return base::Result<void>::success();
}

void produce_chunks(RestoreProducerContext& context) {
    try {
        auto result = produce_all_chunks(context);
        if (!result) {
            context.queue.fail(result.error());
        } else {
            context.queue.close();
        }
    } catch (...) {
        context.queue.fail(
            base::Error{base::ErrorCode::kInternal, "restore producer failed unexpectedly"});
    }
}

void publish_progress(ports::IProgressSink* sink, const RestorePlan& plan,
                      const contracts::TaskPhase phase, const RestoreSummary& summary,
                      const std::uint64_t logical_size) {
    if (sink == nullptr) {
        return;
    }
    sink->publish(contracts::TaskProgress{
        contracts::kTaskProgressSchemaVersion,
        plan.job_id,
        plan.trace_id,
        phase,
        logical_size,
        summary.restored_bytes,
        summary.restored_bytes,
        {},
    });
}

base::Result<RestoreSummary> consume_chunks(RestoreConsumerContext& context) {
    RestoreSummary summary;
    while (true) {
        auto next = context.queue.pop(context.cancellation);
        if (!next) {
            return base::Result<RestoreSummary>::failure(next.error());
        }
        auto chunk_optional = std::move(next).value();
        if (!chunk_optional.has_value()) {
            break;
        }
        auto chunk = std::move(chunk_optional).value();
        auto written = context.sink.write(chunk.descriptor.logical_offset, chunk.payload,
                                          context.cancellation);
        if (!written) {
            return base::Result<RestoreSummary>::failure(written.error());
        }
        summary.restored_bytes += chunk.descriptor.logical_size;
        ++summary.chunk_count;
        publish_progress(context.progress, context.plan, contracts::TaskPhase::kWriting, summary,
                         context.logical_size);
    }
    auto flushed = context.sink.flush(context.cancellation);
    if (!flushed) {
        return base::Result<RestoreSummary>::failure(flushed.error());
    }
    summary.peak_buffered_bytes = context.queue.peak_buffered_bytes();
    publish_progress(context.progress, context.plan, contracts::TaskPhase::kCompleted, summary,
                     context.logical_size);
    return base::Result<RestoreSummary>::success(summary);
}

base::Result<RestoreSummary> consume_safely(RestoreConsumerContext& context,
                                            std::stop_source& local_stop) {
    try {
        auto result = consume_chunks(context);
        if (!result) {
            local_stop.request_stop();
        }
        return result;
    } catch (...) {
        local_stop.request_stop();
        return base::Result<RestoreSummary>::failure(
            base::Error{base::ErrorCode::kInternal, "restore consumer failed unexpectedly"});
    }
}

} // namespace

RestorePipeline::RestorePipeline(ports::IRecoveryPointReader& reader, ports::IBlockSink& sink,
                                 ports::IProgressSink* progress) noexcept
    : reader_(reader), sink_(sink), progress_(progress) {}

base::Result<RestoreSummary> RestorePipeline::run(const RestorePlan& plan,
                                                  const base::CancellationToken& cancellation) {
    const auto plan_validation = validate_plan(plan);
    if (!plan_validation) {
        return base::Result<RestoreSummary>::failure(plan_validation.error());
    }
    const auto preflight = preflight_restore(plan, reader_, sink_);
    if (!preflight) {
        return base::Result<RestoreSummary>::failure(preflight.error());
    }

    detail::BoundedChunkQueue queue(plan.memory_budget_bytes);
    std::stop_source local_stop;
    std::stop_callback external_cancel(cancellation, [&local_stop] { local_stop.request_stop(); });
    RestoreProducerContext producer_context{reader_, queue, local_stop.get_token()};
    RestoreConsumerContext consumer_context{
        plan, sink_, progress_, queue, local_stop.get_token(), reader_.logical_size_bytes()};
    std::jthread producer([&producer_context] { produce_chunks(producer_context); });

    return consume_safely(consumer_context, local_stop);
}

} // namespace aegra::pipeline
