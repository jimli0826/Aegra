#include "service_log_formatter.h"

#include "aegra/base/error.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

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

void redact(Json& value) {
    if (value.is_object()) {
        for (auto& [key, child] : value.items()) {
            if (is_sensitive_key(key)) {
                child = "[REDACTED]";
            } else {
                redact(child);
            }
        }
        return;
    }
    if (value.is_array()) {
        for (auto& child : value) {
            redact(child);
        }
    }
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
    case contracts::ServiceRequestKind::kResolveRecoveryPointChain:
        return "Resolve recovery point chain";
    case contracts::ServiceRequestKind::kPlanDeleteRecoveryPoints:
        return "Plan recovery point deletion";
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
    default:
        return "Unknown command";
    }
}

} // namespace

std::string_view request_kind_name(const contracts::ServiceRequestKind kind) noexcept {
    const auto value = static_cast<std::uint8_t>(kind);
    return value < static_cast<std::uint8_t>(contracts::ServiceRequestKind::kAddRepositoryConnection)
               ? query_kind_name(kind)
               : command_kind_name(kind);
}

std::string readable_code(const std::string_view code) {
    std::string result(code);
    std::ranges::replace_if(result, [](const char value) { return value == '.' || value == '_'; },
                            ' ');
    if (!result.empty()) {
        result.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(result.front())));
    }
    return result;
}

std::string request_log_detail(const contracts::ServiceRequest& request) {
    std::ostringstream stream;
    stream << "operation=" << request_kind_name(request.kind) << "; request_id="
           << request.request_id << "; mode="
           << (contracts::is_service_command_kind(request.kind) ? "command" : "query");
    return stream.str();
}

std::string response_log_detail(const contracts::ServiceResponse& response) {
    std::ostringstream stream;
    stream << "operation=" << request_kind_name(response.request_kind) << "; request_id="
           << response.request_id << "; response=" << response_kind_name(response.kind)
           << "; error=" << readable_code(base::error_code_name(response.boundary_error_code));
    if (!response.message_code.empty()) {
        stream << "; message=" << readable_code(response.message_code) << " ["
               << response.message_code << ']';
    }
    return stream.str();
}

std::string sanitized_interaction_detail(const std::string_view direction,
                                         const std::string_view encoded) {
    try {
        auto value = Json::parse(encoded, nullptr, false);
        if (value.is_discarded()) {
            return std::string(direction) + " interaction data is unavailable: invalid JSON";
        }
        redact(value);
        return std::string(direction) + " interaction data=" + value.dump();
    } catch (...) {
        return std::string(direction) + " interaction data is unavailable";
    }
}

} // namespace aegra::apps::service::detail
