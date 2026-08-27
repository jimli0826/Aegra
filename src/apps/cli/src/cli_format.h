#pragma once

#include "aegra/contracts/repository_query.h"
#include "aegra/contracts/service.h"
#include "aegra/contracts/service_control.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::apps::cli {

[[nodiscard]] std::string_view content_kind_name(contracts::ContentKind kind) noexcept;
[[nodiscard]] std::string_view backup_type_name(contracts::BackupType type) noexcept;
[[nodiscard]] std::string_view job_operation_name(contracts::JobOperation operation) noexcept;
[[nodiscard]] std::string_view job_state_name(contracts::ServiceJobState state) noexcept;
[[nodiscard]] std::string_view service_state_name(contracts::ServiceState state) noexcept;
[[nodiscard]] std::string_view connection_state_name(
    contracts::RepositoryConnectionState state) noexcept;
[[nodiscard]] std::string_view mount_state_name(contracts::MountSessionState state) noexcept;
[[nodiscard]] std::string_view audit_severity_name(contracts::AuditSeverity severity) noexcept;
[[nodiscard]] std::string_view command_disposition_name(
    contracts::CommandDisposition disposition) noexcept;

[[nodiscard]] std::string format_utc_ms(std::uint64_t utc_ms);
[[nodiscard]] std::string format_bytes(std::uint64_t bytes);
[[nodiscard]] std::string format_optional_utc(const std::optional<std::uint64_t>& utc_ms);
[[nodiscard]] std::string format_schedule_trigger(const contracts::ScheduleTrigger& trigger);
[[nodiscard]] std::string format_job_progress(const contracts::JobSummary& job);

void print_status(const contracts::ServiceInfo& info);
void print_schedules(const std::vector<contracts::ScheduleSummary>& items);
void print_jobs(const std::vector<contracts::JobSummary>& items);
void print_repositories(const std::vector<contracts::RepositoryConnectionSummary>& items);
void print_recovery_points(const std::vector<contracts::RecoveryPointSummary>& items);
void print_inventory(const std::vector<contracts::SourceInventoryItem>& items);
void print_events(const std::vector<contracts::AuditEventSummary>& items);
void print_mounts(const std::vector<contracts::MountSessionSummary>& items);
void print_settings(const contracts::ServiceSettings& settings);
void print_acknowledgement(const contracts::CommandAcknowledgement& acknowledgement);
void print_job_line(const contracts::JobSummary& job);

} // namespace aegra::apps::cli
