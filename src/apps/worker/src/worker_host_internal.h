#pragma once

#include "aegra/apps/worker/worker_host.h"
#include "aegra/base/result.h"
#include "aegra/contracts/task_result.h"
#include "aegra/ports/clock.h"

namespace aegra::apps::worker::detail {

class IWorkerTaskExecutor {
  public:
    IWorkerTaskExecutor() = default;
    virtual ~IWorkerTaskExecutor() = default;
    IWorkerTaskExecutor(const IWorkerTaskExecutor&) = delete;
    IWorkerTaskExecutor& operator=(const IWorkerTaskExecutor&) = delete;
    IWorkerTaskExecutor(IWorkerTaskExecutor&&) = delete;
    IWorkerTaskExecutor& operator=(IWorkerTaskExecutor&&) = delete;

    [[nodiscard]] virtual base::Result<contracts::TaskResult>
    execute(const base::CancellationToken& cancellation) = 0;
};

[[nodiscard]] WorkerHostResult
run_worker_host_with_executor(const contracts::JobRequest& job, const ports::IClock& clock,
                              const base::CancellationToken& cancellation,
                              IWorkerTaskExecutor& executor);

} // namespace aegra::apps::worker::detail
