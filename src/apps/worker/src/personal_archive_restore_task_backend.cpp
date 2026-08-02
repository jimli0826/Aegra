#include "personal_archive_restore_task_backend.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/pipeline/restore_pipeline.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace aegra::apps::worker::detail {
namespace {

class PersonalArchiveRestoreTaskBackend final : public IPersonalArchiveRestoreTaskBackend {
  public:
    base::Result<pipeline::RestoreSummary>
    run(const PersonalArchiveRestoreBackendRequest& request,
        const base::CancellationToken& cancellation) override {
        adapters::personal_archive::ArchiveChainOpenRequest open_request;
        open_request.maximum_chain_depth = request.maximum_chain_depth;
        std::vector<std::filesystem::path> protected_sources;
        open_request.layers.reserve(request.layers.size());
        protected_sources.reserve(request.layers.size());
        for (const auto& layer : request.layers) {
            adapters::personal_archive::ArchiveOpenRequest layer_request;
            layer_request.source = layer.source;
            layer_request.password = layer.password;
            layer_request.maximum_chunk_payload_size = request.maximum_chunk_size;
            layer_request.maximum_chunk_logical_size = request.maximum_chunk_size;
            open_request.layers.push_back(std::move(layer_request));
            protected_sources.push_back(layer.source);
        }
        auto reader = adapters::personal_archive::PersonalArchiveChainReader::open(open_request);
        if (!reader) {
            return base::Result<pipeline::RestoreSummary>::failure(reader.error());
        }
        auto sink = adapters::windows_disk::WindowsBlockSink::open(
            {request.target, adapters::windows_disk::WindowsBlockSinkKind::kVolume, std::nullopt,
             std::move(protected_sources)});
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
