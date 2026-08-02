#pragma once

#include "aegra/base/error.h"
#include "aegra/base/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::contracts {

inline constexpr std::uint32_t kTaskResultSchemaVersion = 1;

enum class TaskOutcome : std::uint8_t {
    kSucceeded = 1,
    kSucceededWithWarning = 2,
    kFailed = 3,
    kCancelled = 4,
};

struct TaskResult final {
    std::uint32_t schema_version{kTaskResultSchemaVersion};
    std::string job_id;
    std::string trace_id;
    TaskOutcome outcome{TaskOutcome::kFailed};
    base::ErrorCode error_code{base::ErrorCode::kInternal};
    std::uint64_t logical_bytes{0};
    std::uint64_t stored_bytes{0};
    std::uint64_t chunk_count{0};
    std::string message_code;
    std::vector<std::string> warning_codes;
};

[[nodiscard]] base::Result<void> validate_task_result(const TaskResult& result);

} // namespace aegra::contracts
