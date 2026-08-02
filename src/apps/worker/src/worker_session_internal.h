#pragma once

#include "aegra/apps/worker/worker_session.h"
#include "aegra/contracts/job.h"
#include "aegra/ports/progress.h"

namespace aegra::apps::worker::detail {

class IWorkerSessionTaskRunner {
  public:
    IWorkerSessionTaskRunner() = default;
    virtual ~IWorkerSessionTaskRunner() = default;
    IWorkerSessionTaskRunner(const IWorkerSessionTaskRunner&) = delete;
    IWorkerSessionTaskRunner& operator=(const IWorkerSessionTaskRunner&) = delete;
    IWorkerSessionTaskRunner(IWorkerSessionTaskRunner&&) = delete;
    IWorkerSessionTaskRunner& operator=(IWorkerSessionTaskRunner&&) = delete;

    [[nodiscard]] virtual WorkerHostResult run(const contracts::JobRequest& job,
                                               ports::IProgressSink& progress,
                                               const base::CancellationToken& cancellation) = 0;
};

[[nodiscard]] WorkerExitCode
run_worker_session_with_runner(ports::IMessageChannel& channel,
                               const base::CancellationToken& cancellation,
                               IWorkerSessionTaskRunner& runner);

} // namespace aegra::apps::worker::detail
