#pragma once

#include "aegra/apps/worker/windows_personal_backup_task.h"
#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/pipeline/restore_pipeline.h"

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace aegra::apps::worker::detail {

struct PersonalArchiveRestoreLayer final {
    std::filesystem::path source;
    std::string_view password;
};

struct PersonalArchiveRestoreBackendRequest final {
    std::vector<PersonalArchiveRestoreLayer> layers;
    std::filesystem::path target;
    pipeline::RestorePlan plan;
    std::uint64_t maximum_chunk_size{0};
    std::uint32_t maximum_chain_depth{0};
    ports::IProgressSink* progress{nullptr};
    /// When true, restore disk image (layers are base-first Full→…→tip chain).
    bool disk_restore{false};
    std::uint32_t source_disk_number{0};
    /// Manifest volume_index for volume→volume restore (ignored when disk_restore).
    std::uint32_t source_volume_index{0};
    bool bring_target_online{true};
    bool preserve_disk_signature{true};
    bool auto_expand_last_partition{true};
    std::vector<contracts::RestorePartitionLayoutEdit> partition_layout_edits;
    contracts::VolumeSizePolicy volume_size_policy{contracts::VolumeSizePolicy::kRequireSourceSize};
    std::string shrink_plan_digest; // expected plan_payload_digest hex from eligible preflight
    std::string source_chain_fingerprint; // optional; may be empty if not plumbed yet
    std::string job_id; // for scratch staging path
};

class IPersonalArchiveRestoreTaskBackend {
  public:
    IPersonalArchiveRestoreTaskBackend() = default;
    virtual ~IPersonalArchiveRestoreTaskBackend() = default;
    IPersonalArchiveRestoreTaskBackend(const IPersonalArchiveRestoreTaskBackend&) = delete;
    IPersonalArchiveRestoreTaskBackend& operator=(const IPersonalArchiveRestoreTaskBackend&) = delete;
    IPersonalArchiveRestoreTaskBackend(IPersonalArchiveRestoreTaskBackend&&) = delete;
    IPersonalArchiveRestoreTaskBackend& operator=(IPersonalArchiveRestoreTaskBackend&&) = delete;

    [[nodiscard]] virtual base::Result<pipeline::RestoreSummary>
    run(const PersonalArchiveRestoreBackendRequest& request,
        const base::CancellationToken& cancellation) = 0;
};

[[nodiscard]] base::Result<contracts::TaskResult>
execute_personal_archive_restore_task_with_backend(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context,
    const base::CancellationToken& cancellation, IPersonalArchiveRestoreTaskBackend& backend);

[[nodiscard]] std::unique_ptr<IPersonalArchiveRestoreTaskBackend>
make_personal_archive_restore_task_backend();

} // namespace aegra::apps::worker::detail
