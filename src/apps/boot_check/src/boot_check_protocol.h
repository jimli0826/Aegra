#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/boot_check_job.h"
#include "aegra/contracts/worker_response.h"

#include <string>
#include <string_view>

namespace aegra::apps::boot_check::detail {

/// JSON is the process transport encoding. These functions own the JSON
/// dependency, reject malformed input without exposing parser details, and
/// refuse any field whose name suggests plaintext credential material.
[[nodiscard]] base::Result<contracts::BootCheckJobRequest>
decode_boot_check_job_request(std::string_view encoded);

/// Encodes the shared WorkerResponse wire shape (schema 1).
[[nodiscard]] base::Result<std::string>
encode_boot_check_response(const contracts::WorkerResponse& response);

} // namespace aegra::apps::boot_check::detail
