#include "windows_personal_backup_task_backend.h"

#include <memory>

namespace aegra::apps::worker::detail {
namespace {

class WindowsPersonalBackupTaskBackend final : public IWindowsPersonalBackupTaskBackend {
  public:
    [[nodiscard]] base::Result<WindowsPersonalBackupResult>
    run(const WindowsPersonalBackupRequest& request,
        const base::CancellationToken& cancellation, ports::IProgressSink* progress) override {
        return backup_windows_personal_volumes(request, cancellation, progress);
    }
};

} // namespace

std::unique_ptr<IWindowsPersonalBackupTaskBackend> make_windows_personal_backup_task_backend() {
    return std::make_unique<WindowsPersonalBackupTaskBackend>();
}

} // namespace aegra::apps::worker::detail
