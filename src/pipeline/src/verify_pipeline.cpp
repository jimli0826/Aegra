#include "aegra/pipeline/verify_pipeline.h"

#include "aegra/base/error.h"
#include "aegra/contracts/progress.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace aegra::pipeline {
namespace {

base::Result<void> validate_plan(const VerifyPlan& plan) {
    if (plan.job_id.empty() || plan.trace_id.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "verify plan is incomplete"});
    }
    return base::Result<void>::success();
}

base::Result<void> validate_descriptor(const ports::ChunkDescriptor& descriptor,
                                       const std::uint64_t expected_index,
                                       const std::uint64_t minimum_offset,
                                       const std::uint64_t logical_size) {
    if (descriptor.chunk_index != expected_index || descriptor.logical_size == 0 ||
        descriptor.stored_size != descriptor.logical_size) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "verify chunk descriptor is invalid"});
    }
    if (descriptor.logical_offset < minimum_offset || descriptor.logical_offset > logical_size ||
        descriptor.logical_size > logical_size - descriptor.logical_offset) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "verify chunk range is invalid"});
    }
    return base::Result<void>::success();
}

base::Result<std::uint64_t> descriptor_end(const ports::ChunkDescriptor& descriptor) {
    if (descriptor.logical_offset > (std::numeric_limits<std::uint64_t>::max)() -
                                        descriptor.logical_size) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kCorruptData, "verify chunk range overflows"});
    }
    return base::Result<std::uint64_t>::success(descriptor.logical_offset +
                                                descriptor.logical_size);
}

void publish_progress(ports::IProgressSink* progress, const VerifyPlan& plan,
                      const contracts::TaskPhase phase, const VerifySummary& summary,
                      const std::uint64_t verify_bytes) {
    if (progress == nullptr) {
        return;
    }
    progress->publish(contracts::make_byte_progress(
        plan.job_id, plan.trace_id, phase, verify_bytes, summary.verified_bytes,
        summary.verified_bytes,
        phase == contracts::TaskPhase::kCompleted ? "verify.completed" : "verify.reading"));
}

base::Result<void> verify_chunk(ports::IRecoveryPointReader& reader,
                                const ports::ChunkDescriptor& descriptor,
                                const base::CancellationToken& cancellation) {
    auto chunk = reader.read_chunk(descriptor.chunk_index, cancellation);
    if (!chunk) {
        return base::Result<void>::failure(chunk.error());
    }
    if (chunk.value().descriptor != descriptor ||
        chunk.value().payload.size() != descriptor.logical_size) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "verify chunk payload is inconsistent"});
    }
    return base::Result<void>::success();
}

base::Result<std::vector<ports::ChunkDescriptor>>
preflight(ports::IRecoveryPointReader& reader, const std::uint64_t logical_size) {
    std::vector<ports::ChunkDescriptor> descriptors;
    descriptors.reserve(static_cast<std::size_t>(reader.chunk_count()));
    std::uint64_t minimum_offset = 0;
    for (std::uint64_t index = 0; index < reader.chunk_count(); ++index) {
        auto descriptor = reader.describe_chunk(index);
        if (!descriptor) {
            return base::Result<std::vector<ports::ChunkDescriptor>>::failure(descriptor.error());
        }
        auto valid = validate_descriptor(descriptor.value(), index, minimum_offset, logical_size);
        auto end = descriptor_end(descriptor.value());
        if (!valid || !end) {
            return base::Result<std::vector<ports::ChunkDescriptor>>::failure(
                !valid ? valid.error() : end.error());
        }
        minimum_offset = end.value();
        descriptors.push_back(descriptor.value());
    }
    return base::Result<std::vector<ports::ChunkDescriptor>>::success(std::move(descriptors));
}

base::Result<std::uint64_t>
verify_size(const std::vector<ports::ChunkDescriptor>& descriptors) {
    std::uint64_t total = 0;
    for (const auto& descriptor : descriptors) {
        if (descriptor.logical_size > (std::numeric_limits<std::uint64_t>::max)() - total) {
            return base::Result<std::uint64_t>::failure(
                {base::ErrorCode::kCorruptData, "verify byte total overflows"});
        }
        total += descriptor.logical_size;
    }
    return base::Result<std::uint64_t>::success(total);
}

} // namespace

VerifyPipeline::VerifyPipeline(ports::IRecoveryPointReader& reader,
                               ports::IProgressSink* progress) noexcept
    : reader_(reader), progress_(progress) {}

base::Result<VerifySummary>
VerifyPipeline::run(const VerifyPlan& plan, const base::CancellationToken& cancellation) {
    auto valid_plan = validate_plan(plan);
    if (!valid_plan) {
        return base::Result<VerifySummary>::failure(valid_plan.error());
    }
    VerifySummary summary{reader_.logical_size_bytes(), 0, 0};
    auto descriptors = preflight(reader_, summary.logical_bytes);
    if (!descriptors) {
        return base::Result<VerifySummary>::failure(descriptors.error());
    }
    auto total = verify_size(descriptors.value());
    if (!total) {
        return base::Result<VerifySummary>::failure(total.error());
    }
    for (const auto& descriptor : descriptors.value()) {
        if (cancellation.stop_requested()) {
            return base::Result<VerifySummary>::failure(
                {base::ErrorCode::kCancelled, "verify was cancelled"});
        }
        auto verified = verify_chunk(reader_, descriptor, cancellation);
        if (!verified) {
            return base::Result<VerifySummary>::failure(verified.error());
        }
        summary.verified_bytes += descriptor.logical_size;
        ++summary.chunk_count;
        publish_progress(progress_, plan, contracts::TaskPhase::kReading, summary, total.value());
    }
    publish_progress(progress_, plan, contracts::TaskPhase::kCompleted, summary, total.value());
    return base::Result<VerifySummary>::success(summary);
}

} // namespace aegra::pipeline
