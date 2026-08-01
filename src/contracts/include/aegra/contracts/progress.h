#pragma once

#include "aegra/base/result.h"

#include <cstdint>
#include <string>

namespace aegra::contracts {

inline constexpr std::uint32_t kTaskProgressSchemaVersion = 1;

enum class TaskPhase : std::uint8_t {
    kUnspecified = 0,
    kPreparing = 1,
    kReading = 2,
    kTransforming = 3,
    kWriting = 4,
    kCommitting = 5,
    kCompleted = 6,
};

struct TaskProgress final {
    std::uint32_t schema_version{kTaskProgressSchemaVersion};
    std::string job_id;
    TaskPhase phase{TaskPhase::kPreparing};
    std::uint64_t logical_bytes{0};
    std::uint64_t processed_bytes{0};
    std::uint64_t stored_bytes{0};
    std::string message_code;
};

[[nodiscard]] base::Result<void> validate_task_progress(const TaskProgress& progress);

} // namespace aegra::contracts
