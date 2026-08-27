#pragma once

#include "cli_args.h"
#include "cli_session.h"

#include <cstddef>
#include <optional>
#include <string>

namespace aegra::apps::cli {

inline constexpr std::uint32_t kCliPageSize = 100;
inline constexpr std::size_t kCliMaximumItems = 10'000;

[[nodiscard]] int emit_query(ServiceSession& session, const Options& options,
                             const contracts::ServiceResponse& response);
[[nodiscard]] int fail_missing_id();
[[nodiscard]] int fail_request(const base::Error& error);
[[nodiscard]] base::Result<std::optional<std::string>>
advance_continuation(const std::optional<std::string>& previous,
                     const std::optional<std::string>& next, std::size_t collected);

} // namespace aegra::apps::cli
