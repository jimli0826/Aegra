#pragma once

#include "aegra/apps/worker/worker_host.h"
#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/worker_response.h"

#include <string>
#include <string_view>

namespace aegra::apps::worker {

// JSON is the initial process transport encoding. These functions own all JSON dependency and
// reject malformed input without exposing parser details across the process boundary.
[[nodiscard]] base::Result<contracts::JobRequest>
decode_worker_job_request(std::string_view encoded);

[[nodiscard]] base::Result<std::string>
encode_worker_response(const contracts::WorkerResponse& response);

struct EncodedWorkerResult final {
    WorkerExitCode exit_code{WorkerExitCode::kHostFailure};
    std::string response_json;
};

// Executes one encoded request. Trusted options and concrete capabilities are supplied by the
// composition root and cannot be overridden by the request payload.
[[nodiscard]] base::Result<EncodedWorkerResult> run_windows_personal_backup_worker_request(
    std::string_view encoded_request, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context, const base::CancellationToken& cancellation);

} // namespace aegra::apps::worker
