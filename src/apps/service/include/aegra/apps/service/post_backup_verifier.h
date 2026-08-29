#pragma once

#include "aegra/apps/service/worker_supervisor.h"

#include <memory>

namespace aegra::apps::service {

class IServiceLog;
class WorkerJobService;

/// Bounded asynchronous handoff from a completed backup session to its Verify job.
class PostBackupVerifier final {
  public:
    explicit PostBackupVerifier(IServiceLog* logger = nullptr);
    ~PostBackupVerifier();

    PostBackupVerifier(const PostBackupVerifier&) = delete;
    PostBackupVerifier& operator=(const PostBackupVerifier&) = delete;

    /// Starts the single dispatcher thread. Safe to call once after WorkerJobService exists.
    void start(WorkerJobService& worker_jobs);

    /// Copies the immutable completion snapshot into a bounded queue.
    [[nodiscard]] bool enqueue(const WorkerJobRequest& completed_backup);

    void shutdown() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::apps::service
