#pragma once

#include "aegra/apps/worker/windows_personal_backup.h"
#include "aegra/apps/worker/windows_personal_backup_task.h"

#include <memory>

namespace aegra::apps::worker::detail {

class IWindowsPersonalBackupTaskBackend {
  public:
    IWindowsPersonalBackupTaskBackend() = default;
    virtual ~IWindowsPersonalBackupTaskBackend() = default;
    IWindowsPersonalBackupTaskBackend(const IWindowsPersonalBackupTaskBackend&) = delete;
    IWindowsPersonalBackupTaskBackend& operator=(const IWindowsPersonalBackupTaskBackend&) = delete;
    IWindowsPersonalBackupTaskBackend(IWindowsPersonalBackupTaskBackend&&) = delete;
    IWindowsPersonalBackupTaskBackend& operator=(IWindowsPersonalBackupTaskBackend&&) = delete;

    [[nodiscard]] virtual base::Result<WindowsPersonalBackupResult>
    run(const WindowsPersonalBackupRequest& request,
        const base::CancellationToken& cancellation, ports::IProgressSink* progress) = 0;
};

[[nodiscard]] base::Result<contracts::TaskResult> execute_windows_personal_backup_task_with_backend(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context, const base::CancellationToken& cancellation,
    IWindowsPersonalBackupTaskBackend& backend);

[[nodiscard]] std::unique_ptr<IWindowsPersonalBackupTaskBackend>
make_windows_personal_backup_task_backend();

} // namespace aegra::apps::worker::detail
