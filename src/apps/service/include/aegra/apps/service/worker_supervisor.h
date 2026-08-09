#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/progress.h"
#include "aegra/contracts/service_control.h"
#include "aegra/contracts/worker_response.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace aegra::ports {
class IClock;
class IControlPlaneDatabase;
class IProcessLauncher;
class IRandomSource;
} // namespace aegra::ports

namespace aegra::apps::service {

struct WorkerSupervisorConfig final {
    std::string worker_executable_path;
    std::uint32_t max_concurrent_workers{2};
    std::chrono::seconds default_job_deadline{3600};
    std::chrono::seconds stop_drain_timeout{10};
};

struct WorkerJobRequest final {
    contracts::JobRequest worker_request;
    std::vector<std::string> source_ids;
    std::string repository_connection_id;
    std::optional<std::string> parent_recovery_point_id;
    std::string idempotency_key;
    /// Normalized request identity for idempotent replay (requested fields, not effective type).
    std::string request_fingerprint;
    /// Owning schedule for backup jobs; used to advance last_recovery_point_id after Catalog publish.
    std::string schedule_id;
    std::optional<std::string> backup_archive_key;
    /// Opaque restore preflight token when operation is restore (unique job occupancy).
    std::optional<std::string> preflight_token;
    std::optional<std::string> target_source_id;
    std::chrono::seconds deadline{};
};

using SupervisorProgressCallback =
    std::function<void(std::string_view job_id, const contracts::TaskProgress& progress)>;

using SupervisorCompletionCallback =
    std::function<void(const WorkerJobRequest& request, contracts::ServiceJobState final_state,
                       const contracts::WorkerResponse* response)>;

class WorkerSupervisor {
  public:
    WorkerSupervisor(WorkerSupervisorConfig config, ports::IProcessLauncher& launcher,
                     ports::IControlPlaneDatabase& control_plane, ports::IClock& clock,
                     ports::IRandomSource& random, SupervisorProgressCallback on_progress,
                     SupervisorCompletionCallback on_completion);
    ~WorkerSupervisor();

    WorkerSupervisor(const WorkerSupervisor&) = delete;
    WorkerSupervisor& operator=(const WorkerSupervisor&) = delete;

    [[nodiscard]] base::Result<void> submit(const WorkerJobRequest& request,
                                            base::CancellationToken cancel);

    [[nodiscard]] base::Result<void> cancel_job(std::string_view job_id,
                                                base::CancellationToken cancel);

    void shutdown(const base::CancellationToken& cancel);

    [[nodiscard]] std::uint32_t active_count() const noexcept;

    /// Latest progress for a job: live quantums while running, TaskResult snapshot after
    /// completion (retained so ListJobs reports real bytes instead of synthetic 1/1).
    [[nodiscard]] std::optional<contracts::TaskProgress>
    last_progress(std::string_view job_id) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::apps::service
