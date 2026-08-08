#include "aegra/apps/worker/personal_file_archive_verify_task.h"

#include "worker_task_log.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/base/error.h"
#include "aegra/contracts/file_set.h"
#include "aegra/contracts/progress.h"
#include "aegra/ports/file_recovery_point.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aegra::apps::worker {
namespace detail {
namespace {

base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure({base::ErrorCode::kInvalidArgument, message});
}

base::Result<void> validate_task(const contracts::JobRequest& job,
                                 const WindowsPersonalBackupTaskOptions& options) {
    auto valid_job = contracts::validate_job_request(job);
    if (!valid_job) {
        return valid_job;
    }
    if (job.content_kind != contracts::ContentKind::kFileSet ||
        job.operation != contracts::JobOperation::kVerify || job.source_refs.size() != 1 ||
        !job.target_ref.empty() || job.credential_refs.size() != 1) {
        return invalid(
            "file_set verify requires content_kind=file_set, one source, no target, one credential");
    }
    if (options.memory_budget_bytes == 0) {
        return invalid("file_set verify memory budget is invalid");
    }
    return base::Result<void>::success();
}

std::filesystem::path path_from_utf8(const std::string& value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char item : value) {
        encoded.push_back(static_cast<char8_t>(item));
    }
    return std::filesystem::path(encoded);
}

const char* message_code_for(const base::ErrorCode code) noexcept {
    switch (code) {
    case base::ErrorCode::kCancelled:
        return "verify.cancelled";
    case base::ErrorCode::kUnauthorized:
        return "verify.credential_unavailable";
    case base::ErrorCode::kNotFound:
    case base::ErrorCode::kIoFailure:
        return "verify.source_unavailable";
    case base::ErrorCode::kCorruptData:
        return "verify.corrupt";
    case base::ErrorCode::kInvalidArgument:
        return "verify.invalid_request";
    default:
        return "verify.failed";
    }
}

[[nodiscard]] std::string_view verify_hint_for(const base::ErrorCode code,
                                               const std::string_view message) noexcept {
    if (message.find("password") != std::string_view::npos ||
        code == base::ErrorCode::kUnauthorized) {
        return "Re-enter the archive password and retry";
    }
    if (code == base::ErrorCode::kCorruptData) {
        return "Archive authentication failed; re-backup or pick another recovery point";
    }
    if (code == base::ErrorCode::kNotFound || code == base::ErrorCode::kIoFailure) {
        return "Check archive path, repository connectivity, and file permissions";
    }
    if (code == base::ErrorCode::kCancelled) {
        return "Job was cancelled or deadline expired";
    }
    return "Inspect error_message and retry after fixing the reported condition";
}

contracts::TaskResult failed_result(const contracts::JobRequest& job, const base::ErrorCode code) {
    const auto outcome = code == base::ErrorCode::kCancelled ? contracts::TaskOutcome::kCancelled
                                                             : contracts::TaskOutcome::kFailed;
    contracts::TaskResult result;
    result.job_id = job.job_id;
    result.trace_id = job.trace_id;
    result.outcome = outcome;
    result.error_code = code;
    result.message_code = message_code_for(code);
    return result;
}

contracts::TaskResult completed_result(const contracts::JobRequest& job,
                                       const std::uint64_t logical_bytes,
                                       const std::uint64_t verified_bytes,
                                       const std::uint64_t stream_count) {
    contracts::TaskResult result;
    result.job_id = job.job_id;
    result.trace_id = job.trace_id;
    result.outcome = contracts::TaskOutcome::kSucceeded;
    result.error_code = base::ErrorCode::kNone;
    result.logical_bytes = logical_bytes;
    result.stored_bytes = verified_bytes;
    result.chunk_count = stream_count;
    result.message_code = "verify.completed";
    return result;
}

base::Result<contracts::TaskResult> validated_result(contracts::TaskResult result) {
    auto validation = contracts::validate_task_result(result);
    if (!validation) {
        return base::Result<contracts::TaskResult>::failure(validation.error());
    }
    return base::Result<contracts::TaskResult>::success(std::move(result));
}

void publish_progress(const contracts::JobRequest& job, ports::IProgressSink* progress,
                      const contracts::TaskPhase phase, const std::uint64_t verified_bytes,
                      const std::uint64_t logical_bytes, const std::string& message_code) {
    if (progress == nullptr) {
        return;
    }
    progress->publish(contracts::make_byte_progress(job.job_id, job.trace_id, phase, logical_bytes,
                                                    verified_bytes, verified_bytes, message_code));
}

class EmptyPasswordSecret final : public ports::IResolvedSecret {
  public:
    [[nodiscard]] std::string_view view() const noexcept override {
        return {};
    }
};

[[nodiscard]] base::Result<std::unique_ptr<ports::IResolvedSecret>>
resolve_verify_secret(const contracts::JobRequest& job, ports::ICredentialResolver& credentials,
                      const base::CancellationToken& cancellation) {
    if (job.credential_refs.front().value.empty()) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::success(
            std::make_unique<EmptyPasswordSecret>());
    }
    auto resolved = credentials.resolve(job.credential_refs.front(), cancellation);
    if (!resolved || resolved.value() == nullptr || resolved.value()->view().empty()) {
        const auto code = !resolved && resolved.error().code == base::ErrorCode::kCancelled
                              ? base::ErrorCode::kCancelled
                              : base::ErrorCode::kUnauthorized;
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(
            {code, !resolved ? resolved.error().message : "archive credential is unavailable"});
    }
    return resolved;
}

[[nodiscard]] base::Result<std::uint64_t>
verify_stream(ports::IFileRecoveryPointReader& reader, const contracts::FileStreamDesc& stream,
              const std::size_t budget, const base::CancellationToken& cancellation) {
    if (stream.logical_size == 0) {
        return base::Result<std::uint64_t>::success(0);
    }
    std::uint64_t verified = 0;
    std::vector<std::byte> buffer(budget);
    while (verified < stream.logical_size) {
        if (cancellation.stop_requested()) {
            return base::Result<std::uint64_t>::failure(
                {base::ErrorCode::kCancelled, "file_set verify cancelled"});
        }
        const auto remaining = stream.logical_size - verified;
        const auto request_size =
            static_cast<std::uint64_t>((std::min)(static_cast<std::uint64_t>(buffer.size()), remaining));
        ports::FileStreamReadRequest request;
        request.stream_index = stream.stream_index;
        request.offset = verified;
        request.size = request_size;
        auto read = reader.read_stream(request, std::span<std::byte>(buffer.data(),
                                                                     static_cast<std::size_t>(request_size)),
                                       cancellation);
        if (!read) {
            return base::Result<std::uint64_t>::failure(read.error());
        }
        if (read.value() == 0) {
            return base::Result<std::uint64_t>::failure(
                {base::ErrorCode::kCorruptData, "file stream ended before logical size"});
        }
        if (static_cast<std::uint64_t>(read.value()) > remaining) {
            return base::Result<std::uint64_t>::failure(
                {base::ErrorCode::kCorruptData, "file stream read exceeded logical size"});
        }
        verified += static_cast<std::uint64_t>(read.value());
    }
    return base::Result<std::uint64_t>::success(verified);
}

void log_verify_request(WorkerTaskLog* log, const contracts::JobRequest& job,
                        const WindowsPersonalBackupTaskOptions& options) {
    if (log == nullptr) {
        return;
    }
    log->section("Job");
    log->field("job_id", job.job_id);
    log->field("trace_id", job.trace_id);
    log->field("operation", "verify");
    log->section("Request");
    log->field("content_kind", "file_set");
    log->field_bytes("memory_budget", options.memory_budget_bytes);
    log->field("password", job.credential_refs.front().value.empty() ? "empty" : "present");
}

void log_verify_success(WorkerTaskLog* log, const contracts::TaskResult& result,
                        const std::chrono::steady_clock::time_point started) {
    if (log == nullptr) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    log->section("Result");
    log->field("outcome", "succeeded");
    log->field_bytes("logical_bytes", result.logical_bytes);
    log->field_bytes("verified_bytes", result.stored_bytes);
    log->field_u64("streams", result.chunk_count);
    log->field("elapsed", format_duration_ms(elapsed));
}

struct StreamVerifyTotals final {
    std::uint64_t logical_bytes{0};
    std::uint64_t verified_bytes{0};
    std::uint64_t streams_verified{0};
};

[[nodiscard]] base::Result<StreamVerifyTotals>
verify_all_streams(ports::IFileRecoveryPointReader& reader, const contracts::JobRequest& job,
                   const std::size_t budget, ports::IProgressSink* progress, WorkerTaskLog* log,
                   const base::CancellationToken& cancellation) {
    ScopedStage stage(log, "verify_streams");
    StreamVerifyTotals totals;
    std::unordered_set<std::uint32_t> seen_streams;
    for (std::uint64_t entry_id = 1; entry_id <= reader.entry_count(); ++entry_id) {
        if (cancellation.stop_requested()) {
            const base::Error error{base::ErrorCode::kCancelled, "file_set verify cancelled"};
            stage.fail(error, "describe_entry", verify_hint_for(error.code, error.message));
            return base::Result<StreamVerifyTotals>::failure(error);
        }
        auto entry = reader.describe_entry(entry_id, cancellation);
        if (!entry) {
            if (entry.error().code == base::ErrorCode::kNotFound) {
                continue;
            }
            stage.fail(entry.error(), "describe_entry",
                       verify_hint_for(entry.error().code, entry.error().message));
            return base::Result<StreamVerifyTotals>::failure(entry.error());
        }
        for (const auto& stream : entry.value().streams) {
            if (!seen_streams.insert(stream.stream_index).second) {
                continue;
            }
            if (totals.logical_bytes >
                (std::numeric_limits<std::uint64_t>::max)() - stream.logical_size) {
                const base::Error error{base::ErrorCode::kCorruptData,
                                        "file_set verify logical size overflow"};
                stage.fail(error, "stream_size", verify_hint_for(error.code, error.message));
                return base::Result<StreamVerifyTotals>::failure(error);
            }
            totals.logical_bytes += stream.logical_size;
            auto verified = verify_stream(reader, stream, budget, cancellation);
            if (!verified) {
                stage.fail(verified.error(), "read_stream",
                           verify_hint_for(verified.error().code, verified.error().message));
                return base::Result<StreamVerifyTotals>::failure(verified.error());
            }
            totals.verified_bytes += verified.value();
            ++totals.streams_verified;
            publish_progress(job, progress, contracts::TaskPhase::kReading, totals.verified_bytes,
                             totals.logical_bytes, "verify.reading");
        }
    }
    if (totals.streams_verified != reader.stream_count()) {
        const base::Error error{base::ErrorCode::kCorruptData,
                                "file_set verify stream count mismatch"};
        stage.fail(error, "stream_count", verify_hint_for(error.code, error.message));
        return base::Result<StreamVerifyTotals>::failure(error);
    }
    stage.note_bytes("verified_bytes", totals.verified_bytes);
    stage.note_bytes("logical_bytes", totals.logical_bytes);
    stage.note_u64("streams", totals.streams_verified);
    return base::Result<StreamVerifyTotals>::success(totals);
}

[[nodiscard]] base::Result<contracts::TaskResult>
run_file_set_verify(const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
                    const WindowsPersonalBackupTaskContext& context,
                    const base::CancellationToken& cancellation) {
    auto task_log = WorkerTaskLog::open("verify", job.job_id);
    const auto started = std::chrono::steady_clock::now();
    log_verify_request(task_log.get(), job, options);
    if (cancellation.stop_requested() ||
        (job.deadline_utc_ms > 0 && context.clock.now_utc_ms() >= job.deadline_utc_ms)) {
        return validated_result(failed_result(job, base::ErrorCode::kCancelled));
    }
    publish_progress(job, context.progress, contracts::TaskPhase::kPreparing, 0, 0,
                     "verify.preparing");

    std::unique_ptr<ports::IResolvedSecret> secret;
    {
        ScopedStage stage(task_log.get(), "resolve_credentials");
        auto resolved = resolve_verify_secret(job, context.credentials, cancellation);
        if (!resolved) {
            stage.fail(resolved.error(), "resolve_secret",
                       verify_hint_for(resolved.error().code, resolved.error().message));
            return validated_result(failed_result(job, resolved.error().code));
        }
        stage.note("password", resolved.value()->view().empty() ? "empty" : "present");
        secret = std::move(resolved).value();
    }

    std::unique_ptr<adapters::personal_archive::PersonalFileArchiveReader> reader;
    {
        ScopedStage stage(task_log.get(), "open_archive");
        adapters::personal_archive::ArchiveOpenRequest request;
        request.source = path_from_utf8(job.source_refs.front());
        request.password = secret->view();
        request.maximum_chunk_payload_size = options.memory_budget_bytes;
        request.maximum_chunk_logical_size = options.memory_budget_bytes;
        auto opened = adapters::personal_archive::PersonalFileArchiveReader::open(request);
        if (!opened) {
            stage.fail(opened.error(), "PersonalFileArchiveReader::open",
                       verify_hint_for(opened.error().code, opened.error().message));
            return validated_result(failed_result(job, opened.error().code));
        }
        reader = std::move(opened).value();
        stage.note_u64("entry_count", reader->entry_count());
        stage.note_u64("stream_count", reader->stream_count());
        stage.note("index_generation", reader->index_root_digest());
    }

    const auto budget = static_cast<std::size_t>(
        (std::min)(options.memory_budget_bytes,
                   static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())));
    auto totals = verify_all_streams(*reader, job, budget == 0 ? 1 : budget, context.progress,
                                     task_log.get(), cancellation);
    if (!totals) {
        return validated_result(failed_result(job, totals.error().code));
    }
    publish_progress(job, context.progress, contracts::TaskPhase::kCompleted,
                     totals.value().verified_bytes, totals.value().logical_bytes,
                     "verify.completed");
    auto result = validated_result(completed_result(job, totals.value().logical_bytes,
                                                    totals.value().verified_bytes,
                                                    totals.value().streams_verified));
    if (result) {
        log_verify_success(task_log.get(), result.value(), started);
    }
    return result;
}

} // namespace

base::Result<contracts::TaskResult>
run_file_set_verify_task(const contracts::JobRequest& job,
                         const WindowsPersonalBackupTaskOptions& options,
                         const WindowsPersonalBackupTaskContext& context,
                         const base::CancellationToken& cancellation) {
    if (auto valid = validate_task(job, options); !valid) {
        return base::Result<contracts::TaskResult>::failure(valid.error());
    }
    return run_file_set_verify(job, options, context, cancellation);
}

} // namespace detail

base::Result<contracts::TaskResult> execute_personal_file_archive_verify_task(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context,
    const base::CancellationToken& cancellation) {
    try {
        return detail::run_file_set_verify_task(job, options, context, cancellation);
    } catch (const std::exception&) {
        return base::Result<contracts::TaskResult>::failure(
            {base::ErrorCode::kInternal, "file_set verify entry failed unexpectedly"});
    }
}

} // namespace aegra::apps::worker
