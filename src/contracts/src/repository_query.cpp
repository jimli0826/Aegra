#include "aegra/contracts/repository_query.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>

namespace aegra::contracts {
namespace {

constexpr std::size_t kMaximumContinuationTokenBytes = 1'024;

[[nodiscard]] base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure({base::ErrorCode::kInvalidArgument, message});
}

[[nodiscard]] bool canonical_uuid(const std::string_view value) noexcept {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
        value[23] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        const auto character = value[index];
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return value[14] >= '1' && value[14] <= '8' &&
           (value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
}

[[nodiscard]] bool valid_token(const std::optional<std::string>& token) noexcept {
    if (!token) {
        return true;
    }
    return !token->empty() && token->size() <= kMaximumContinuationTokenBytes &&
           std::ranges::all_of(*token, [](const unsigned char character) {
               return character >= 0x21U && character <= 0x7EU;
           });
}

[[nodiscard]] bool known_backup_type(const PersonalBackupType type) noexcept {
    return type == PersonalBackupType::kFull || type == PersonalBackupType::kIncremental ||
           type == PersonalBackupType::kDifferential;
}

[[nodiscard]] bool known_chain_state(const RecoveryPointChainState state) noexcept {
    return state == RecoveryPointChainState::kComplete ||
           state == RecoveryPointChainState::kIncomplete;
}

} // namespace

base::Result<void> validate_recovery_point_list_request(const RecoveryPointListRequest& request) {
    if (request.maximum_results == 0 ||
        request.maximum_results > kMaximumRecoveryPointPageResults ||
        !valid_token(request.continuation_token)) {
        return invalid("recovery point list request is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_recovery_point_summary(const RecoveryPointSummary& summary) {
    if (!canonical_uuid(summary.file_uuid) || !canonical_uuid(summary.backup_set_uuid) ||
        (summary.parent_uuid && !canonical_uuid(*summary.parent_uuid)) ||
        !known_backup_type(summary.backup_type) || !known_chain_state(summary.chain_state)) {
        return invalid("recovery point summary identity or state is invalid");
    }
    if ((summary.backup_type == PersonalBackupType::kFull) != !summary.parent_uuid ||
        (summary.parent_uuid && *summary.parent_uuid == summary.file_uuid)) {
        return invalid("recovery point summary parent is invalid");
    }
    if (summary.backup_type == PersonalBackupType::kFull &&
        summary.chain_state != RecoveryPointChainState::kComplete) {
        return invalid("full recovery point chain state is invalid");
    }
    constexpr auto maximum_wire_integer =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if (summary.created_utc_ms > maximum_wire_integer ||
        summary.logical_size_bytes > maximum_wire_integer ||
        summary.stored_size_bytes > maximum_wire_integer) {
        return invalid("recovery point summary integer exceeds the service wire range");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_recovery_point_page(const RecoveryPointPage& page) {
    if (!valid_token(page.continuation_token) ||
        page.items.size() > kMaximumRecoveryPointPageResults) {
        return invalid("recovery point page bounds are invalid");
    }
    if (page.state == RepositoryCatalogState::kNotConfigured) {
        return page.repository_uuid.empty() && page.items.empty() && !page.continuation_token
                   ? base::Result<void>::success()
                   : invalid("unconfigured repository page is invalid");
    }
    if (page.state != RepositoryCatalogState::kCatalogReady ||
        !canonical_uuid(page.repository_uuid)) {
        return invalid("repository catalog page state is invalid");
    }
    std::string_view previous;
    for (const auto& item : page.items) {
        auto valid = validate_recovery_point_summary(item);
        if (!valid || (!previous.empty() && item.file_uuid <= previous)) {
            return invalid("recovery point page items are invalid or unsorted");
        }
        previous = item.file_uuid;
    }
    return base::Result<void>::success();
}

} // namespace aegra::contracts
