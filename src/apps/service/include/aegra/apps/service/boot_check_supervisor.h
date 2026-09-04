#pragma once

#include "aegra/base/result.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/process_launcher.h"
#include "aegra/ports/repository_storage.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace aegra::apps::service {

class IServiceLog;

/// Identity of one BootCheck run. boot_check_job_id ([a-z0-9-], <=64) becomes
/// the host job id, the VM name suffix, and the private job directory name.
struct BootCheckDispatch final {
    std::string boot_check_job_id;
    std::string recovery_point_id;
    std::string repository_connection_id;
    std::string schedule_id;
    contracts::BootCheckHypervisor hypervisor{contracts::BootCheckHypervisor::kVirtualBox};
};

struct BootCheckRunResult final {
    bool succeeded{false};
    std::string message_code;
};

/// Runs at most one AegraBootCheck.exe at a time. try_start resolves the
/// base-first volume chain from the Catalog, stages the request file under the
/// Service data directory, launches the host (bounded by a kill watchdog), and
/// parses the WorkerResponse from captured stdout on a background thread.
/// take_result returns a finished run's terminal outcome exactly once.
class BootCheckSupervisor final {
  public:
    struct Options final {
        /// Trusted absolute AegraBootCheck.exe path; empty = unavailable.
        std::filesystem::path host_executable_path;
        std::filesystem::path data_directory;
    };

    BootCheckSupervisor(Options options, ports::IProcessLauncher& launcher,
                        ports::IControlPlaneDatabase& control_plane,
                        ports::IRepositoryStorageFactory& storage_factory, ports::IClock& clock,
                        IServiceLog* logger);
    ~BootCheckSupervisor();

    BootCheckSupervisor(const BootCheckSupervisor&) = delete;
    BootCheckSupervisor& operator=(const BootCheckSupervisor&) = delete;
    BootCheckSupervisor(BootCheckSupervisor&&) = delete;
    BootCheckSupervisor& operator=(BootCheckSupervisor&&) = delete;

    [[nodiscard]] bool available() const noexcept;
    /// Invoked on the runner thread right after a run's result becomes
    /// consumable, so the coordinator can collect it without waiting for the
    /// next scan. Set once during composition, before any dispatch.
    void set_completion_observer(std::function<void()> observer);
    /// Launches `AegraBootCheck.exe --scavenge` once in the background to
    /// remove orphaned job directories and isolated VMs from previous Service
    /// instances. Dispatch is blocked until the scavenge finishes so a new job
    /// directory can never race the cleanup.
    void begin_scavenge();
    /// False when unavailable, shutting down, scavenging, or a run is active.
    [[nodiscard]] bool try_start(const BootCheckDispatch& dispatch);
    /// Terminal result for the given run, consumed on read.
    [[nodiscard]] std::optional<BootCheckRunResult> take_result(std::string_view boot_check_job_id);
    /// True while the given run is dispatched (running or finished-unconsumed).
    [[nodiscard]] bool is_tracking(std::string_view boot_check_job_id) const;
    void shutdown() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::apps::service
