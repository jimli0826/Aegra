#pragma once

#include "aegra/base/result.h"

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

struct RecoveryPointListRequest final {
    std::uint32_t maximum_results{50};
    std::optional<std::string> continuation_token;
};

struct RecoveryPointSummary final {
    std::string file_uuid;
    std::string backup_set_uuid;
    std::optional<std::string> parent_uuid;
    PersonalBackupType backup_type{PersonalBackupType::kFull};
    RecoveryPointChainState chain_state{RecoveryPointChainState::kIncomplete};
    std::uint64_t created_utc_ms{0};
    std::uint64_t logical_size_bytes{0};
    std::uint64_t stored_size_bytes{0};
    std::uint32_t source_count{0};
    bool has_sidecar{false};
};

struct RecoveryPointPage final {
    RepositoryCatalogState state{RepositoryCatalogState::kNotConfigured};
    std::string repository_uuid;
    std::vector<RecoveryPointSummary> items;
    std::optional<std::string> continuation_token;
};

[[nodiscard]] base::Result<void>
validate_recovery_point_list_request(const RecoveryPointListRequest& request);
[[nodiscard]] base::Result<void>
validate_recovery_point_summary(const RecoveryPointSummary& summary);
[[nodiscard]] base::Result<void> validate_recovery_point_page(const RecoveryPointPage& page);

} // namespace aegra::contracts
