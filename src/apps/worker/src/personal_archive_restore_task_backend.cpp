#include "personal_archive_restore_task_backend.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/format/manifest.h"
#include "aegra/pipeline/restore_pipeline.h"

#include <memory>
#include <optional>

namespace aegra::apps::worker::detail {
namespace {

class PersonalArchiveRestoreTaskBackend final : public IPersonalArchiveRestoreTaskBackend {
  public:
    base::Result<pipeline::RestoreSummary>
    run(const PersonalArchiveRestoreBackendRequest& request,
        const base::CancellationToken& cancellation) override {
        adapters::personal_archive::ArchiveOpenRequest open_request;
        open_request.source = request.source;
        open_request.password = request.password;
        open_request.maximum_chunk_payload_size = request.maximum_chunk_size;
        open_request.maximum_chunk_logical_size = request.maximum_chunk_size;
        auto reader = adapters::personal_archive::PersonalArchiveReader::open(open_request);
        if (!reader) {
            return base::Result<pipeline::RestoreSummary>::failure(reader.error());
        }
        if (reader.value()->identity().backup_type != format::BackupType::kFull) {
            return base::Result<pipeline::RestoreSummary>::failure(
                {base::ErrorCode::kConflict, "single archive restore requires a full backup"});
        }
        auto sink = adapters::windows_disk::WindowsBlockSink::open(
            {request.target, adapters::windows_disk::WindowsBlockSinkKind::kVolume, std::nullopt,
             request.source});
        if (!sink) {
            return base::Result<pipeline::RestoreSummary>::failure(sink.error());
        }
        pipeline::RestorePipeline restore(*reader.value(), *sink.value(), request.progress);
        return restore.run(request.plan, cancellation);
    }
};

} // namespace

std::unique_ptr<IPersonalArchiveRestoreTaskBackend>
make_personal_archive_restore_task_backend() {
    return std::make_unique<PersonalArchiveRestoreTaskBackend>();
}

} // namespace aegra::apps::worker::detail
