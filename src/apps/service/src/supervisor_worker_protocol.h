#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/worker_session.h"

#include <string>
#include <string_view>

namespace aegra::apps::service {

[[nodiscard]] base::Result<std::string>
encode_supervisor_job_request(const contracts::JobRequest& request);

[[nodiscard]] base::Result<contracts::WorkerEvent>
decode_supervisor_worker_event(std::string_view json_text);

[[nodiscard]] base::Result<std::string>
encode_supervisor_worker_command(const contracts::WorkerCommand& command);

} // namespace aegra::apps::service
