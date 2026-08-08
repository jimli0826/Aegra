#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"

#include <cstdint>
#include <optional>
#include <string>

namespace aegra::contracts {

inline constexpr std::uint32_t kTaskProgressSchemaVersion = 4;

enum class TaskPhase : std::uint8_t {
    kUnspecified = 0,
    kPreparing = 1,
    kReading = 2,
    kTransforming = 3,
    kWriting = 4,
    kCommitting = 5,
    kCompleted = 6,
};

/// Progress event. When `logical_bytes` is nullopt, byte percentage is unknown
/// (file enumeration); use discovered_entries / processed_entries instead.
struct TaskProgress final {
    std::uint32_t schema_version{kTaskProgressSchemaVersion};
    std::string job_id;
    std::string trace_id;
    TaskPhase phase{TaskPhase::kPreparing};
    std::optional<std::uint64_t> logical_bytes;
    std::uint64_t processed_bytes{0};
    std::uint64_t stored_bytes{0};
    std::uint64_t discovered_entries{0};
    std::uint64_t processed_entries{0};
    std::string message_code;
};

[[nodiscard]] base::Result<void> validate_task_progress(const TaskProgress& progress);

/// Helper for volume pipelines that always know the byte total.
[[nodiscard]] inline TaskProgress make_byte_progress(std::string job_id, std::string trace_id,
                                                     TaskPhase phase, std::uint64_t logical_bytes,
                                                     std::uint64_t processed_bytes,
                                                     std::uint64_t stored_bytes,
                                                     std::string message_code) {
    TaskProgress progress;
    progress.job_id = std::move(job_id);
    progress.trace_id = std::move(trace_id);
    progress.phase = phase;
    progress.logical_bytes = logical_bytes;
    progress.processed_bytes = processed_bytes;
    progress.stored_bytes = stored_bytes;
    progress.message_code = std::move(message_code);
    return progress;
}

} // namespace aegra::contracts
