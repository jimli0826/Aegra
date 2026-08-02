#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/task_result.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/credential.h"
#include "aegra/ports/progress.h"
#include "aegra/ports/random.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace aegra::apps::worker {

struct WindowsPersonalBackupTaskOptions final {
    std::uint32_t block_size_bytes{0};
    std::uint32_t chunk_size_bytes{0};
    std::size_t memory_budget_bytes{0};
    std::uint64_t split_size_bytes{0};
    std::uint64_t kdf_opslimit{3};
    std::uint64_t kdf_memlimit_bytes{256ULL * 1024ULL * 1024ULL};
    std::uint32_t maximum_restore_chain_depth{128};
    std::string application_version;
    std::string hostname;
};

struct WindowsPersonalBackupTaskContext final {
    ports::ICredentialResolver& credentials;
    ports::IRandomSource& random;
    const ports::IClock& clock;
    ports::IProgressSink* progress{nullptr};
};

// Contract validation failures reject the request. Once accepted, execution failures are returned
// as a validated TaskResult with stable, non-sensitive message codes.
[[nodiscard]] base::Result<contracts::TaskResult> execute_windows_personal_backup_task(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context, const base::CancellationToken& cancellation);

} // namespace aegra::apps::worker
