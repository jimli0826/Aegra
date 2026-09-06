#include "cli_format.h"

#include "cli_io.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace aegra::apps::cli {
namespace {

[[nodiscard]] std::string pad(const std::string& value, const std::size_t width) {
    if (value.size() >= width) {
        return value;
    }
    return value + std::string(width - value.size(), ' ');
}

void print_table(const std::vector<std::string>& headers,
                 const std::vector<std::vector<std::string>>& rows) {
    if (headers.empty()) {
        return;
    }
    std::vector<std::size_t> widths(headers.size(), 0);
    for (std::size_t column = 0; column < headers.size(); ++column) {
        widths[column] = headers[column].size();
    }
    for (const auto& row : rows) {
        for (std::size_t column = 0; column < headers.size() && column < row.size(); ++column) {
            widths[column] = (std::max)(widths[column], row[column].size());
        }
    }
    std::string header_line;
    for (std::size_t column = 0; column < headers.size(); ++column) {
        if (column != 0) {
            header_line += "  ";
        }
        header_line += pad(headers[column], widths[column]);
    }
    write_line(header_line);
    if (rows.empty()) {
        write_line("(none)");
        return;
    }
    for (const auto& row : rows) {
        std::string line;
        for (std::size_t column = 0; column < headers.size(); ++column) {
            if (column != 0) {
                line += "  ";
            }
            const auto cell = column < row.size() ? row[column] : std::string{};
            line += pad(cell, widths[column]);
        }
        write_line(line);
    }
}

[[nodiscard]] std::string join_csv(const std::vector<std::string>& values) {
    std::string joined;
    for (const auto& value : values) {
        if (!joined.empty()) {
            joined += ',';
        }
        joined += value;
    }
    return joined.empty() ? "-" : joined;
}

[[nodiscard]] std::string hhmm(const std::uint16_t minutes) {
    char buffer[8]{};
    std::snprintf(buffer, sizeof(buffer), "%02u:%02u",
                  static_cast<unsigned>(minutes / 60U), static_cast<unsigned>(minutes % 60U));
    return buffer;
}

[[nodiscard]] std::string weekday_names(const std::uint8_t mask) {
    static constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    std::string names;
    for (std::uint8_t bit = 0; bit < 7; ++bit) {
        if ((mask & static_cast<std::uint8_t>(1U << bit)) == 0) {
            continue;
        }
        if (!names.empty()) {
            names += ',';
        }
        names += kDays[bit];
    }
    return names.empty() ? "-" : names;
}

[[nodiscard]] std::string month_days(const std::uint32_t mask) {
    std::string days;
    for (std::uint32_t bit = 0; bit < 31; ++bit) {
        if ((mask & (1U << bit)) == 0) {
            continue;
        }
        if (!days.empty()) {
            days += ',';
        }
        days += std::to_string(bit + 1U);
    }
    return days.empty() ? "-" : days;
}

[[nodiscard]] std::string yes_no(const bool value) {
    return value ? "yes" : "no";
}

} // namespace

std::string_view content_kind_name(const contracts::ContentKind kind) noexcept {
    switch (kind) {
    case contracts::ContentKind::kVolumeSet:
        return "volume_set";
    case contracts::ContentKind::kFileSet:
        return "file_set";
    }
    return "unknown";
}

std::string_view backup_type_name(const contracts::BackupType type) noexcept {
    switch (type) {
    case contracts::BackupType::kFull:
        return "full";
    case contracts::BackupType::kIncremental:
        return "incremental";
    case contracts::BackupType::kDifferential:
        return "differential";
    }
    return "unknown";
}

std::string_view job_operation_name(const contracts::JobOperation operation) noexcept {
    switch (operation) {
    case contracts::JobOperation::kBackup:
        return "backup";
    case contracts::JobOperation::kRestore:
        return "restore";
    case contracts::JobOperation::kVerify:
        return "verify";
    case contracts::JobOperation::kExport:
        return "export";
    }
    return "unknown";
}

std::string_view job_state_name(const contracts::ServiceJobState state) noexcept {
    switch (state) {
    case contracts::ServiceJobState::kQueued:
        return "queued";
    case contracts::ServiceJobState::kRunning:
        return "running";
    case contracts::ServiceJobState::kCancelling:
        return "cancelling";
    case contracts::ServiceJobState::kSucceeded:
        return "succeeded";
    case contracts::ServiceJobState::kFailed:
        return "failed";
    case contracts::ServiceJobState::kCancelled:
        return "cancelled";
    case contracts::ServiceJobState::kInterrupted:
        return "interrupted";
    }
    return "unknown";
}

std::string_view service_state_name(const contracts::ServiceState state) noexcept {
    switch (state) {
    case contracts::ServiceState::kStarting:
        return "starting";
    case contracts::ServiceState::kReady:
        return "ready";
    case contracts::ServiceState::kStopping:
        return "stopping";
    }
    return "unknown";
}

std::string_view
connection_state_name(const contracts::RepositoryConnectionState state) noexcept {
    switch (state) {
    case contracts::RepositoryConnectionState::kAvailable:
        return "available";
    case contracts::RepositoryConnectionState::kUnavailable:
        return "unavailable";
    }
    return "unknown";
}

std::string_view mount_state_name(const contracts::MountSessionState state) noexcept {
    switch (state) {
    case contracts::MountSessionState::kMounting:
        return "mounting";
    case contracts::MountSessionState::kMounted:
        return "mounted";
    case contracts::MountSessionState::kUnmounting:
        return "unmounting";
    case contracts::MountSessionState::kFailed:
        return "failed";
    }
    return "unknown";
}

std::string_view audit_severity_name(const contracts::AuditSeverity severity) noexcept {
    switch (severity) {
    case contracts::AuditSeverity::kInformation:
        return "info";
    case contracts::AuditSeverity::kWarning:
        return "warning";
    case contracts::AuditSeverity::kError:
        return "error";
    case contracts::AuditSeverity::kCritical:
        return "critical";
    }
    return "unknown";
}

std::string_view
command_disposition_name(const contracts::CommandDisposition disposition) noexcept {
    switch (disposition) {
    case contracts::CommandDisposition::kAccepted:
        return "accepted";
    case contracts::CommandDisposition::kReplayed:
        return "replayed";
    }
    return "unknown";
}

std::string format_utc_ms(const std::uint64_t utc_ms) {
    if (utc_ms == 0) {
        return "-";
    }
    ULARGE_INTEGER filetime_value{};
    constexpr std::uint64_t kUnixEpochOffsetMs = 11'644'473'600'000ULL;
    if (utc_ms > (std::numeric_limits<std::uint64_t>::max() / 10'000ULL) - kUnixEpochOffsetMs) {
        return "-";
    }
    filetime_value.QuadPart = (utc_ms + kUnixEpochOffsetMs) * 10'000ULL;
    FILETIME utc{};
    utc.dwLowDateTime = filetime_value.LowPart;
    utc.dwHighDateTime = filetime_value.HighPart;
    FILETIME local{};
    if (FileTimeToLocalFileTime(&utc, &local) == FALSE) {
        return "-";
    }
    SYSTEMTIME wall{};
    if (FileTimeToSystemTime(&local, &wall) == FALSE) {
        return "-";
    }
    char buffer[20]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u", wall.wYear, wall.wMonth,
                  wall.wDay, wall.wHour, wall.wMinute);
    return buffer;
}

std::string format_bytes(const std::uint64_t bytes) {
    char buffer[32]{};
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        std::snprintf(buffer, sizeof(buffer), "%.1f GiB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MiB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

std::string format_optional_utc(const std::optional<std::uint64_t>& utc_ms) {
    return utc_ms ? format_utc_ms(*utc_ms) : "-";
}

std::string format_schedule_trigger(const contracts::ScheduleTrigger& trigger) {
    std::string times;
    for (const auto minute : trigger.local_minutes_of_day) {
        if (!times.empty()) {
            times += ',';
        }
        times += hhmm(minute);
    }
    if (times.empty()) {
        times = "-";
    }
    switch (trigger.kind) {
    case contracts::ScheduleTriggerKind::kDaily:
        return "daily " + times;
    case contracts::ScheduleTriggerKind::kWeekly:
        return "weekly " + weekday_names(trigger.weekday_mask) + ' ' + times;
    case contracts::ScheduleTriggerKind::kMonthly:
        return "monthly " + month_days(trigger.day_of_month_mask) + ' ' + times;
    }
    return "unknown";
}

std::string format_job_progress(const contracts::JobSummary& job) {
    if (!job.progress) {
        return "-";
    }
    if (job.progress->logical_bytes && *job.progress->logical_bytes > 0) {
        const auto percent = (job.progress->processed_bytes * 100ULL) / *job.progress->logical_bytes;
        return std::to_string(percent > 100 ? 100 : percent) + "%";
    }
    if (job.progress->discovered_entries > 0) {
        return std::to_string(job.progress->processed_entries) + "/" +
               std::to_string(job.progress->discovered_entries);
    }
    return format_bytes(job.progress->processed_bytes);
}

void print_status(const contracts::ServiceInfo& info) {
    write_line(std::string("state            ") + std::string(service_state_name(info.state)));
    write_line("service_version  " + info.service_version);
    write_line("api_version      " + std::to_string(info.api_version));
    write_line("capabilities     " + join_csv(info.capabilities));
}

void print_schedules(const std::vector<contracts::ScheduleSummary>& items) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        rows.push_back({item.schedule_id, item.display_name,
                        std::string(content_kind_name(item.content_kind)),
                        yes_no(item.enabled), format_schedule_trigger(item.trigger),
                        format_optional_utc(item.next_run_utc_ms),
                        yes_no(item.encryption_enabled)});
    }
    print_table({"ID", "NAME", "KIND", "ENABLED", "TRIGGER", "NEXT RUN", "ENC"}, rows);
}

void print_jobs(const std::vector<contracts::JobSummary>& items) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        rows.push_back({item.job_id, std::string(job_operation_name(item.operation)),
                        std::string(job_state_name(item.state)),
                        item.schedule_id.value_or("-"), format_job_progress(item),
                        format_utc_ms(item.created_utc_ms), item.message_code});
    }
    print_table({"ID", "OP", "STATE", "SCHEDULE", "PROGRESS", "CREATED", "MESSAGE"}, rows);
}

void print_repositories(const std::vector<contracts::RepositoryConnectionSummary>& items) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        rows.push_back({item.connection_id, item.display_name, item.locator,
                        std::string(connection_state_name(item.state)), yes_no(item.is_default)});
    }
    print_table({"ID", "NAME", "LOCATOR", "STATE", "DEFAULT"}, rows);
}

void print_recovery_points(const std::vector<contracts::RecoveryPointSummary>& items) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        rows.push_back({item.file_uuid,
                        std::string(content_kind_name(item.content_kind)),
                        std::string(backup_type_name(static_cast<contracts::BackupType>(
                            static_cast<std::uint8_t>(item.backup_type)))),
                        format_utc_ms(item.created_utc_ms), format_bytes(item.logical_size_bytes),
                        format_bytes(item.stored_size_bytes)});
    }
    print_table({"ID", "KIND", "TYPE", "CREATED", "LOGICAL", "STORED"}, rows);
}

void print_inventory(const std::vector<contracts::SourceInventoryItem>& items) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        rows.push_back({item.source_id, item.display_name, item.mount_letter,
                        format_bytes(item.capacity_bytes), yes_no(item.is_system),
                        yes_no(item.is_selectable)});
    }
    print_table({"ID", "NAME", "LETTER", "SIZE", "SYSTEM", "SELECTABLE"}, rows);
}

void print_events(const std::vector<contracts::AuditEventSummary>& items) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        rows.push_back({item.event_id, format_utc_ms(item.created_utc_ms),
                        std::string(audit_severity_name(item.severity)), item.message_code,
                        item.correlation_id});
    }
    print_table({"ID", "CREATED", "SEVERITY", "MESSAGE", "CORRELATION"}, rows);
}

void print_mounts(const std::vector<contracts::MountSessionSummary>& items) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        rows.push_back({item.session_id, item.recovery_point_id,
                        std::string(mount_state_name(item.state)), item.mount_point,
                        item.message_code});
    }
    print_table({"ID", "RECOVERY POINT", "STATE", "MOUNT", "MESSAGE"}, rows);
}

void print_settings(const contracts::ServiceSettings& settings) {
    write_line("job_retention_months  " + std::to_string(settings.job_retention_months));
    write_line(std::string("default_hypervisor    ") +
               (settings.default_boot_check_hypervisor ==
                        contracts::BootCheckHypervisor::kHyperV
                    ? "hyper_v"
                    : "virtual_box"));
    write_line("boot_check_cpu_count  " + std::to_string(settings.boot_check_cpu_count));
    write_line("boot_check_memory_mib " + std::to_string(settings.boot_check_memory_mib));
    write_line("boot_check_concurrency " + std::to_string(settings.boot_check_concurrency));
    write_line("effective_concurrency " +
               std::to_string(settings.boot_check_effective_concurrency));
    write_line(std::string("verify_scope          ") +
               (settings.verify_scope == contracts::VerifyScope::kFullChain ? "full_chain"
                                                                             : "single_file"));
    write_line("verify_concurrency    " + std::to_string(settings.verify_concurrency));
    write_line("host_logical_cpus     " + std::to_string(settings.host_logical_cpu_count));
    write_line("host_memory_mib       " + std::to_string(settings.host_physical_memory_mib));
    write_line("updated               " + format_utc_ms(settings.updated_utc_ms));
}

void print_acknowledgement(const contracts::CommandAcknowledgement& acknowledgement) {
    write_line(std::string("disposition  ") +
               std::string(command_disposition_name(acknowledgement.disposition)));
    write_line("command_id   " + acknowledgement.command_id);
    write_line("resource_id  " + acknowledgement.resource_id.value_or("-"));
}

void print_job_line(const contracts::JobSummary& job) {
    write_line(std::string(job.job_id) + "  " + std::string(job_state_name(job.state)) + "  " +
               format_job_progress(job) + "  " + job.message_code);
}

} // namespace aegra::apps::cli
