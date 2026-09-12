#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegra::contracts {

inline constexpr std::uint32_t kMaximumRecoveryPointPageResults = 100;

enum class RepositoryCatalogState : std::uint8_t {
    kNotConfigured = 1,
    kCatalogReady = 2,
};

enum class PersonalBackupType : std::uint8_t {
    kFull = 1,
    kIncremental = 2,
    kDifferential = 3,
};

enum class RecoveryPointChainState : std::uint8_t {
    kComplete = 1,
    kIncomplete = 2,
};

/// Terminal outcome of the latest Verify or BootCheck run the Service recorded
/// for a recovery point (ADR-0033). Control-plane projection: present only on
/// the Service wire, absent for Catalog-only in-process queries.
enum class RecoveryPointCheckState : std::uint8_t {
    kSucceeded = 1,
    kFailed = 2,
    kCancelled = 3,
    kInterrupted = 4,
};

[[nodiscard]] constexpr bool
is_known_recovery_point_check_state(const RecoveryPointCheckState state) noexcept {
    return state == RecoveryPointCheckState::kSucceeded ||
           state == RecoveryPointCheckState::kFailed ||
           state == RecoveryPointCheckState::kCancelled ||
           state == RecoveryPointCheckState::kInterrupted;
}

struct RecoveryPointCheckStatus final {
    RecoveryPointCheckState state{RecoveryPointCheckState::kFailed};
    /// Stable result code of the run (verify.* / bootcheck.* / job.*).
    std::string message_code;
    std::string job_id;
    std::uint64_t completed_utc_ms{0};
};

struct RecoveryPointListRequest final {
    std::uint32_t maximum_results{50};
    std::optional<std::string> continuation_token;
};

struct RecoveryPointSummary final {
    std::string file_uuid;
    std::string backup_set_uuid;
    std::optional<std::string> parent_uuid;
    PersonalBackupType backup_type{PersonalBackupType::kFull};
    ContentKind content_kind{ContentKind::kVolumeSet};
    RecoveryPointChainState chain_state{RecoveryPointChainState::kIncomplete};
    std::uint64_t created_utc_ms{0};
    std::uint64_t logical_size_bytes{0};
    std::uint64_t stored_size_bytes{0};
    /// volume_set: DEDUP entry count from Catalog/Footer; file_set always 0.
    std::uint64_t deduplicated_block_count{0};
    /// volume_set: expanded DEDUP logical bytes; file_set always 0.
    std::uint64_t deduplicated_logical_bytes{0};
    std::uint32_t source_count{0};
    bool has_sidecar{false};
    /// Latest recorded Verify / BootCheck outcome; absent = never run (N/A).
    std::optional<RecoveryPointCheckStatus> verify_check;
    std::optional<RecoveryPointCheckStatus> boot_check;
};

struct RecoveryPointPage final {
    RepositoryCatalogState state{RepositoryCatalogState::kNotConfigured};
    std::string repository_uuid;
    std::vector<RecoveryPointSummary> items;
    std::optional<std::string> continuation_token;
    /// In-process only (not on the Service wire). Keys of skipped invalid catalog entries.
    std::vector<std::string> skipped_catalog_entry_keys;
};

[[nodiscard]] base::Result<void>
validate_recovery_point_list_request(const RecoveryPointListRequest& request);
[[nodiscard]] base::Result<void>
validate_recovery_point_summary(const RecoveryPointSummary& summary);
[[nodiscard]] base::Result<void> validate_recovery_point_page(const RecoveryPointPage& page);

} // namespace aegra::contracts
