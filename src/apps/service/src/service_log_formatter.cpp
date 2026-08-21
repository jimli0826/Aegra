#include "service_log_formatter.h"

#include "aegra/apps/service/service_host.h"
#include "aegra/base/error.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>

namespace aegra::apps::service::detail {
namespace {

using Json = nlohmann::json;

[[nodiscard]] bool is_sensitive_key(const std::string_view key) noexcept {
    return key.find("authorization") != std::string_view::npos ||
           key.find("cookie") != std::string_view::npos ||
           key.find("credential") != std::string_view::npos ||
           key.find("password") != std::string_view::npos ||
           key.find("secret") != std::string_view::npos ||
           key.find("token") != std::string_view::npos || key.ends_with("_key");
}

[[nodiscard]] bool should_omit_trace_key(const std::string_view key) noexcept {
    return key == "request_id" || is_sensitive_key(key);
}

void omit_trace_fields(Json& value) {
    if (value.is_object()) {
        std::vector<std::string> omit;
        for (auto& [key, child] : value.items()) {
            if (should_omit_trace_key(key)) {
                omit.push_back(key);
            } else {
                omit_trace_fields(child);
            }
        }
        for (const auto& key : omit) {
            value.erase(key);
        }
        return;
    }
    if (value.is_array()) {
        for (auto& child : value) {
            omit_trace_fields(child);
        }
    }
}

[[nodiscard]] std::optional<std::uint8_t> read_u8_field(const Json& value,
                                                        const char* key) noexcept {
    try {
        if (!value.is_object() || !value.contains(key)) {
            return std::nullopt;
        }
        const auto& field = value.at(key);
        if (field.is_number_unsigned()) {
            const auto number = field.get<std::uint64_t>();
            if (number > (std::numeric_limits<std::uint8_t>::max)()) {
                return std::nullopt;
            }
            return static_cast<std::uint8_t>(number);
        }
        if (field.is_number_integer()) {
            const auto number = field.get<std::int64_t>();
            if (number < 0 ||
                number > static_cast<std::int64_t>((std::numeric_limits<std::uint8_t>::max)())) {
                return std::nullopt;
            }
            return static_cast<std::uint8_t>(number);
        }
    } catch (...) {
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view response_kind_name(const contracts::ServiceResponseKind kind) {
    switch (kind) {
    case contracts::ServiceResponseKind::kQueryResult:
        return "Query result";
    case contracts::ServiceResponseKind::kCommandAccepted:
        return "Command accepted";
    case contracts::ServiceResponseKind::kRequestFailed:
        return "Request failed";
    }
    return "Unknown response";
}

[[nodiscard]] std::string_view query_kind_name(const contracts::ServiceRequestKind kind) {
    switch (kind) {
    case contracts::ServiceRequestKind::kGetServiceInfo:
        return "Get service information";
    case contracts::ServiceRequestKind::kListRecoveryPoints:
        return "List recovery points";
    case contracts::ServiceRequestKind::kListRepositoryConnections:
        return "List repository connections";
    case contracts::ServiceRequestKind::kListSourceInventory:
        return "List backup sources";
    case contracts::ServiceRequestKind::kListJobs:
        return "List jobs";
    case contracts::ServiceRequestKind::kListSchedules:
        return "List schedules";
    case contracts::ServiceRequestKind::kListEvents:
        return "List events";
    case contracts::ServiceRequestKind::kListMountSessions:
        return "List mount sessions";
    case contracts::ServiceRequestKind::kPrepareRestore:
        return "Prepare restore";
    case contracts::ServiceRequestKind::kAnalyzeNtfsShrink:
        return "Analyze NTFS shrink";
    case contracts::ServiceRequestKind::kResolveRecoveryPointChain:
        return "Resolve recovery point chain";
    case contracts::ServiceRequestKind::kPlanDeleteRecoveryPoints:
        return "Plan recovery point deletion";
    case contracts::ServiceRequestKind::kGetRecoveryPointLayout:
        return "Get recovery point layout";
    case contracts::ServiceRequestKind::kBrowseFileSources:
        return "Browse file sources";
    case contracts::ServiceRequestKind::kListRecoveryPointEntries:
        return "List recovery point entries";
    case contracts::ServiceRequestKind::kPrepareFileRestore:
        return "Prepare file restore";
    case contracts::ServiceRequestKind::kGetServiceSettings:
        return "Get service settings";
    case contracts::ServiceRequestKind::kListRepositoryDirectories:
        return "List repository directories";
    default:
        return "Unknown query";
    }
}

[[nodiscard]] std::string_view command_kind_name(const contracts::ServiceRequestKind kind) {
    switch (kind) {
    case contracts::ServiceRequestKind::kAddRepositoryConnection:
        return "Add repository connection";
    case contracts::ServiceRequestKind::kImportRepositoryConnection:
        return "Import repository connection";
    case contracts::ServiceRequestKind::kConnectRepositoryLocation:
        return "Connect repository location";
    case contracts::ServiceRequestKind::kTestRepositoryConnection:
        return "Test repository connection";
    case contracts::ServiceRequestKind::kSetDefaultRepository:
        return "Set default repository";
    case contracts::ServiceRequestKind::kRemoveRepositoryConnection:
        return "Remove repository connection";
    case contracts::ServiceRequestKind::kStartBackup:
        return "Start backup";
    case contracts::ServiceRequestKind::kCancelJob:
        return "Cancel job";
    case contracts::ServiceRequestKind::kStartVerify:
        return "Start verification";
    case contracts::ServiceRequestKind::kStartRestore:
        return "Start restore";
    case contracts::ServiceRequestKind::kMountRecoveryPoint:
        return "Mount recovery point";
    case contracts::ServiceRequestKind::kUnmountSession:
        return "Unmount session";
    case contracts::ServiceRequestKind::kUpsertSchedule:
        return "Create or update schedule";
    case contracts::ServiceRequestKind::kDeleteSchedule:
        return "Delete schedule";
    case contracts::ServiceRequestKind::kSubscribeTaskEvents:
        return "Subscribe to task events";
    case contracts::ServiceRequestKind::kAcknowledgeEvents:
        return "Acknowledge events";
    case contracts::ServiceRequestKind::kExecuteDeletePlan:
        return "Execute recovery point deletion";
    case contracts::ServiceRequestKind::kStartFileRestore:
        return "Start file restore";
    case contracts::ServiceRequestKind::kUpdateServiceSettings:
        return "Update service settings";
    default:
        return "Unknown command";
    }
}

[[nodiscard]] std::string_view
local_request_kind_name(const contracts::ServiceRequestKind kind) noexcept {
    const auto value = static_cast<std::uint8_t>(kind);
    return value < static_cast<std::uint8_t>(
                       contracts::ServiceRequestKind::kAddRepositoryConnection)
               ? query_kind_name(kind)
               : command_kind_name(kind);
}

[[nodiscard]] std::string_view interaction_kind_label(const std::string_view direction,
                                                     const Json& value) noexcept {
    if (direction == "Inbound") {
        if (const auto kind = read_u8_field(value, "kind"); kind.has_value()) {
            return local_request_kind_name(static_cast<contracts::ServiceRequestKind>(*kind));
        }
        return "Unknown request";
    }
    if (const auto request_kind = read_u8_field(value, "request_kind"); request_kind.has_value()) {
        return local_request_kind_name(static_cast<contracts::ServiceRequestKind>(*request_kind));
    }
    if (const auto kind = read_u8_field(value, "kind"); kind.has_value()) {
        return response_kind_name(static_cast<contracts::ServiceResponseKind>(*kind));
    }
    return "Unknown response";
}

[[nodiscard]] std::string format_interaction_detail(const std::string_view direction,
                                                    const std::string_view encoded) {
    const auto prefix = direction == "Inbound" ? "request" : "response";
    try {
        auto value = Json::parse(encoded, nullptr, false);
        if (value.is_discarded()) {
            return std::string(prefix) + ": unavailable (invalid JSON)";
        }
        const auto kind_label = interaction_kind_label(direction, value);
        omit_trace_fields(value);
        return std::string(prefix) + ": " + std::string(kind_label) + " " + value.dump();
    } catch (...) {
        return std::string(prefix) + ": unavailable";
    }
}

} // namespace

std::string_view request_kind_name(const contracts::ServiceRequestKind kind) noexcept {
    return local_request_kind_name(kind);
}

std::string readable_code(const std::string_view code) {
    std::string result(code);
    std::ranges::replace_if(
        result, [](const char value) { return value == '.' || value == '_'; }, ' ');
    if (!result.empty()) {
        result.front() =
            static_cast<char>(std::toupper(static_cast<unsigned char>(result.front())));
    }
    return result;
}

std::string request_log_detail(const contracts::ServiceRequest& request) {
    std::ostringstream stream;
    stream << "operation=" << request_kind_name(request.kind)
           << "; request_id=" << request.request_id
           << "; mode=" << (contracts::is_service_command_kind(request.kind) ? "command" : "query");
    return stream.str();
}

std::string response_log_detail(const contracts::ServiceResponse& response) {
    std::ostringstream stream;
    stream << "operation=" << request_kind_name(response.request_kind)
           << "; request_id=" << response.request_id
           << "; response=" << response_kind_name(response.kind)
           << "; error=" << readable_code(base::error_code_name(response.boundary_error_code));
    if (!response.message_code.empty()) {
        stream << "; message=" << readable_code(response.message_code) << " ["
               << response.message_code << ']';
    }
    return stream.str();
}

std::string sanitized_interaction_detail(const std::string_view direction,
                                         const std::string_view encoded) {
    return format_interaction_detail(direction, encoded);
}

std::string format_log_bytes(const std::uint64_t bytes) {
    constexpr double kKib = 1024.0;
    constexpr double kMib = kKib * 1024.0;
    constexpr double kGib = kMib * 1024.0;
    char human[64]{};
    if (bytes >= static_cast<std::uint64_t>(kGib)) {
        std::snprintf(human, sizeof(human), "%.2f GiB", static_cast<double>(bytes) / kGib);
    } else if (bytes >= static_cast<std::uint64_t>(kMib)) {
        std::snprintf(human, sizeof(human), "%.2f MiB", static_cast<double>(bytes) / kMib);
    } else if (bytes >= static_cast<std::uint64_t>(kKib)) {
        std::snprintf(human, sizeof(human), "%.2f KiB", static_cast<double>(bytes) / kKib);
    } else {
        std::snprintf(human, sizeof(human), "%llu B", static_cast<unsigned long long>(bytes));
        return human;
    }
    char full[96]{};
    std::snprintf(full, sizeof(full), "%s (%llu bytes)", human,
                  static_cast<unsigned long long>(bytes));
    return full;
}

std::string format_log_duration(const std::chrono::milliseconds elapsed) {
    char buffer[64]{};
    if (elapsed.count() < 1000) {
        std::snprintf(buffer, sizeof(buffer), "%lld ms",
                      static_cast<long long>(elapsed.count()));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.3f s",
                      static_cast<double>(elapsed.count()) / 1000.0);
    }
    return buffer;
}

std::string pad_log_key(const std::string_view key) {
    constexpr std::size_t kWidth = 36;
    if (key.size() == kWidth) {
        return std::string(key);
    }
    if (key.size() < kWidth) {
        std::string out(key);
        out.append(kWidth - key.size(), ' ');
        return out;
    }
    std::string out;
    out.reserve(kWidth);
    out.append(key.data(), kWidth - 3);
    out.append("...");
    return out;
}

void PlainFlowLog::line(const std::string_view text) noexcept {
    if (logger_ == nullptr) {
        return;
    }
    try {
        logger_->write(ServiceLogLevel::kInfo, kPlainLogEvent, text);
    } catch (...) {
    }
}

void PlainFlowLog::blank() noexcept { line(""); }

void PlainFlowLog::section(const std::string_view title) noexcept {
    blank();
    line(std::string("[") + std::string(title) + "]");
}

void PlainFlowLog::field(const std::string_view key, const std::string_view value) noexcept {
    line(std::string("  ") + pad_log_key(key) + " : " + std::string(value));
}

void PlainFlowLog::field_u64(const std::string_view key, const std::uint64_t value) noexcept {
    field(key, std::to_string(value));
}

void PlainFlowLog::field_bool(const std::string_view key, const bool value) noexcept {
    field(key, value ? "true" : "false");
}

void PlainFlowLog::field_bytes(const std::string_view key, const std::uint64_t bytes) noexcept {
    field(key, format_log_bytes(bytes));
}

void PlainFlowLog::stage_begin(const std::string_view stage) noexcept {
    blank();
    line(std::string("[Stage: ") + std::string(stage) + "] begin");
}

void PlainFlowLog::stage_ok(const std::string_view stage,
                            const std::chrono::milliseconds elapsed) noexcept {
    line(std::string("[Stage: ") + std::string(stage) + "] OK (" + format_log_duration(elapsed) +
         ")");
}

void PlainFlowLog::stage_fail(const std::string_view stage,
                              const std::chrono::milliseconds elapsed,
                              const std::string_view error_code,
                              const std::string_view message_code) noexcept {
    line(std::string("[Stage: ") + std::string(stage) + "] FAILED (" +
         format_log_duration(elapsed) + ")");
    if (!error_code.empty()) {
        field("error_code", error_code);
    }
    if (!message_code.empty()) {
        field("message_code", message_code);
    }
}

} // namespace aegra::apps::service::detail
