#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/service_control.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegra::apps::cli {

enum class Command : std::uint8_t {
    kHelp = 0,
    kStatus = 1,
    kScheduleList = 2,
    kScheduleRun = 3,
    kScheduleDelete = 4,
    kJobList = 5,
    kJobCancel = 6,
    kJobWait = 7,
    kRepositoryList = 8,
    kRecoveryPointList = 9,
    kInventoryList = 10,
    kEventList = 11,
    kMountList = 12,
    kSettingsGet = 13,
};

struct Options final {
    Command command{Command::kHelp};
    bool json{false};
    std::uint32_t timeout_ms{30'000};
    std::uint32_t wait_timeout_ms{3'600'000};
    std::optional<std::string> id;
    std::optional<contracts::BackupType> backup_type;
    std::optional<bool> enabled_filter;
    contracts::JobListScope job_scope{contracts::JobListScope::kActive};
    std::optional<contracts::JobOperation> job_operation;
    std::optional<std::string> connection_id;
    bool wait{false};
};

[[nodiscard]] base::Result<Options> parse_args(int argc, wchar_t** argv);
void print_help();

} // namespace aegra::apps::cli
