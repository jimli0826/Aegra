#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/progress.h"
#include "aegra/contracts/worker_response.h"

#include <cstdint>
#include <optional>
#include <string>

namespace aegra::contracts {

inline constexpr std::uint32_t kWorkerCommandSchemaVersion = 1;
inline constexpr std::uint32_t kWorkerEventSchemaVersion = 1;

enum class WorkerCommandKind : std::uint8_t {
    kCancel = 1,
};

struct WorkerCommand final {
    std::uint32_t schema_version{kWorkerCommandSchemaVersion};
    std::string job_id;
    std::string trace_id;
    WorkerCommandKind kind{WorkerCommandKind::kCancel};
};

enum class WorkerEventKind : std::uint8_t {
    kProgress = 1,
    kResult = 2,
};

struct WorkerEvent final {
    std::uint32_t schema_version{kWorkerEventSchemaVersion};
    std::string job_id;
    std::string trace_id;
    WorkerEventKind kind{WorkerEventKind::kResult};
    std::optional<TaskProgress> progress;
    std::optional<WorkerResponse> response;
};

[[nodiscard]] base::Result<void> validate_worker_command(const WorkerCommand& command);
[[nodiscard]] base::Result<void> validate_worker_event(const WorkerEvent& event);

} // namespace aegra::contracts
