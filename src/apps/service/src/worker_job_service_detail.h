#pragma once

// Shared helpers for worker_job_service*.cpp (Service composition root only).

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/random.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace aegra::apps::service::worker_job_detail {

[[nodiscard]] base::Result<std::string> random_id(std::string_view prefix,
                                                  ports::IRandomSource& random,
                                                  base::CancellationToken cancellation);

[[nodiscard]] base::Result<std::filesystem::path> path_from_utf8(std::string_view value);

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);

[[nodiscard]] contracts::CommandAcknowledgement
acknowledgement(std::string command_id, contracts::CommandDisposition disposition,
                std::optional<std::string> resource_id);

[[nodiscard]] base::Result<std::string>
resolve_archive_absolute_path(const std::string& locator, const std::string& archive_main_key);

} // namespace aegra::apps::service::worker_job_detail
