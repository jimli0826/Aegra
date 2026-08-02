#pragma once

#include "aegra/base/error.h"
#include "aegra/base/result.h"
#include "aegra/contracts/task_result.h"

#include <cstdint>
#include <optional>
#include <string>

namespace aegra::contracts {

inline constexpr std::uint32_t kWorkerResponseSchemaVersion = 1;

enum class WorkerResponseKind : std::uint8_t {
    kTaskResult = 1,
    kRequestRejected = 2,
    kHostFailure = 3,
};

struct WorkerResponse final {
    std::uint32_t schema_version{kWorkerResponseSchemaVersion};
    std::string job_id;
    std::string trace_id;
    WorkerResponseKind kind{WorkerResponseKind::kHostFailure};
    base::ErrorCode boundary_error_code{base::ErrorCode::kInternal};
    std::string message_code;
    std::optional<TaskResult> task_result;
};

[[nodiscard]] base::Result<void> validate_worker_response(const WorkerResponse& response);

} // namespace aegra::contracts
