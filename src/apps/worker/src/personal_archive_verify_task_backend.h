#pragma once

#include "aegra/apps/worker/windows_personal_backup_task.h"
#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/pipeline/verify_pipeline.h"

#include <filesystem>
#include <memory>
#include <string_view>

namespace aegra::apps::worker::detail {

class IPersonalArchiveVerifyTaskBackend {
  public:
    IPersonalArchiveVerifyTaskBackend() = default;
    virtual ~IPersonalArchiveVerifyTaskBackend() = default;
    IPersonalArchiveVerifyTaskBackend(const IPersonalArchiveVerifyTaskBackend&) = delete;
    IPersonalArchiveVerifyTaskBackend& operator=(const IPersonalArchiveVerifyTaskBackend&) = delete;
    IPersonalArchiveVerifyTaskBackend(IPersonalArchiveVerifyTaskBackend&&) = delete;
    IPersonalArchiveVerifyTaskBackend& operator=(IPersonalArchiveVerifyTaskBackend&&) = delete;

    [[nodiscard]] virtual base::Result<pipeline::VerifySummary>
    run(const std::filesystem::path& source, std::string_view password,
        const pipeline::VerifyPlan& plan, const WindowsPersonalBackupTaskOptions& options,
        const base::CancellationToken& cancellation, ports::IProgressSink* progress) = 0;
};

[[nodiscard]] base::Result<contracts::TaskResult> execute_personal_archive_verify_task_with_backend(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context,
    const base::CancellationToken& cancellation, IPersonalArchiveVerifyTaskBackend& backend);

[[nodiscard]] std::unique_ptr<IPersonalArchiveVerifyTaskBackend>
make_personal_archive_verify_task_backend();

} // namespace aegra::apps::worker::detail
