#include "personal_archive_verify_task_backend.h"

#include "worker_task_log.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/pipeline/verify_pipeline.h"

#include <memory>
#include <string_view>

namespace aegra::apps::worker::detail {
namespace {

[[nodiscard]] std::string_view verify_stage_hint(const base::Error& error) noexcept {
    if (error.code == base::ErrorCode::kUnauthorized) {
        return "Re-enter the archive password and retry";
    }
    if (error.code == base::ErrorCode::kCorruptData) {
        return "Archive authentication failed; re-backup or pick another recovery point";
    }
    if (error.code == base::ErrorCode::kNotFound || error.code == base::ErrorCode::kIoFailure) {
        return "Check archive path, repository connectivity, and file permissions";
    }
    return {};
}

class PersonalArchiveVerifyTaskBackend final : public IPersonalArchiveVerifyTaskBackend {
  public:
    base::Result<pipeline::VerifySummary>
    run(const std::filesystem::path& source, const std::string_view password,
        const pipeline::VerifyPlan& plan, const WindowsPersonalBackupTaskOptions& options,
        const base::CancellationToken& cancellation, ports::IProgressSink* progress) override {
        std::unique_ptr<adapters::personal_archive::PersonalArchiveReader> reader;
        {
            ScopedStage stage(WorkerTaskLog::active(), "open_archive");
            stage.note("source", path_display(source));
            stage.note_bytes("memory_budget", options.memory_budget_bytes);
            adapters::personal_archive::ArchiveOpenRequest request;
            request.source = source;
            request.password = password;
            request.maximum_chunk_payload_size = options.memory_budget_bytes;
            request.maximum_chunk_logical_size = options.memory_budget_bytes;
            auto opened = adapters::personal_archive::PersonalArchiveReader::open(request);
            if (!opened) {
                stage.fail(opened.error(), "PersonalArchiveReader::open",
                           verify_stage_hint(opened.error()));
                return base::Result<pipeline::VerifySummary>::failure(opened.error());
            }
            reader = std::move(opened).value();
            stage.note_bytes("logical_size", reader->logical_size_bytes());
            stage.note_u64("chunk_count", reader->chunk_count());
            stage.note_u64("volumes_in_manifest", reader->manifest().volumes.size());
            stage.note_u64("disks_in_manifest", reader->manifest().disks.size());
        }

        {
            ScopedStage stage(WorkerTaskLog::active(), "verify_pipeline");
            stage.note_u64("chunk_count", reader->chunk_count());
            stage.note_bytes("logical_size", reader->logical_size_bytes());
            pipeline::VerifyPipeline verify(*reader, progress);
            auto summary = verify.run(plan, cancellation);
            if (!summary) {
                stage.fail(summary.error(), "pipeline_run", verify_stage_hint(summary.error()));
                return summary;
            }
            stage.note_bytes("verified_bytes", summary.value().verified_bytes);
            stage.note_bytes("logical_bytes", summary.value().logical_bytes);
            stage.note_u64("chunks", summary.value().chunk_count);
            return summary;
        }
    }
};

} // namespace

std::unique_ptr<IPersonalArchiveVerifyTaskBackend>
make_personal_archive_verify_task_backend() {
    return std::make_unique<PersonalArchiveVerifyTaskBackend>();
}

} // namespace aegra::apps::worker::detail
