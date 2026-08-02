#pragma once

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

} // namespace aegra::apps::worker
