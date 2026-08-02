#include "personal_archive_verify_task_backend.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/pipeline/verify_pipeline.h"

#include <memory>

namespace aegra::apps::worker::detail {
namespace {

class PersonalArchiveVerifyTaskBackend final : public IPersonalArchiveVerifyTaskBackend {
  public:
    base::Result<pipeline::VerifySummary>
    run(const std::filesystem::path& source, const std::string_view password,
        const pipeline::VerifyPlan& plan, const WindowsPersonalBackupTaskOptions& options,
        const base::CancellationToken& cancellation, ports::IProgressSink* progress) override {
        adapters::personal_archive::ArchiveOpenRequest request;
        request.source = source;
        request.password = password;
        request.maximum_chunk_payload_size = options.memory_budget_bytes;
        request.maximum_chunk_logical_size = options.memory_budget_bytes;
        auto reader = adapters::personal_archive::PersonalArchiveReader::open(request);
        if (!reader) {
            return base::Result<pipeline::VerifySummary>::failure(reader.error());
        }
        pipeline::VerifyPipeline verify(*reader.value(), progress);
        return verify.run(plan, cancellation);
    }
};

} // namespace

std::unique_ptr<IPersonalArchiveVerifyTaskBackend>
make_personal_archive_verify_task_backend() {
    return std::make_unique<PersonalArchiveVerifyTaskBackend>();
}

} // namespace aegra::apps::worker::detail
