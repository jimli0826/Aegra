#pragma once

#include "aegra/base/error.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegra::contracts {

inline constexpr std::uint32_t kTaskResultSchemaVersion = 4;

enum class TaskOutcome : std::uint8_t {
    kSucceeded = 1,
    kSucceededWithWarning = 2,
    kFailed = 3,
    kCancelled = 4,
};

// Forward declare to avoid job.h cycle; values match contracts::BackupType.
enum class BackupType : std::uint8_t;

struct TaskResult final {
    std::uint32_t schema_version{kTaskResultSchemaVersion};
    std::string job_id;
    std::string trace_id;
    TaskOutcome outcome{TaskOutcome::kFailed};
    base::ErrorCode error_code{base::ErrorCode::kInternal};
    std::uint64_t logical_bytes{0};
    std::uint64_t stored_bytes{0};
    std::uint64_t chunk_count{0};
    std::uint64_t entry_count{0};
    std::uint64_t stream_count{0};
    /// volume_set: DEDUP entry count from committed Footer; file_set always 0.
    std::uint64_t deduplicated_block_count{0};
    /// volume_set: expanded plaintext bytes represented by DEDUP entries; file_set always 0.
    std::uint64_t deduplicated_logical_bytes{0};
    std::string message_code;
    std::vector<std::string> warning_codes;
    std::optional<PartialRestoreStats> partial_restore;
    /// file_set backup: requested vs effective type and non-sensitive downgrade reason.
    std::optional<std::uint8_t> requested_backup_type;
    std::optional<std::uint8_t> effective_backup_type;
    std::optional<std::string> effective_parent_uuid;
    std::optional<IncrementalDowngradeReason> incremental_downgrade_reason;
};

[[nodiscard]] base::Result<void> validate_task_result(const TaskResult& result);

} // namespace aegra::contracts
