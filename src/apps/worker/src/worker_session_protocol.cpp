#include "aegra/apps/worker/worker_protocol.h"

#include "aegra/base/error.h"
#include "aegra/contracts/progress.h"
#include "aegra/contracts/task_result.h"
#include "aegra/contracts/worker_response.h"
#include "aegra/contracts/worker_session.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::apps::worker {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaximumCommandBytes = std::size_t{64} * 1024U;

base::Result<contracts::WorkerCommand> invalid_command(const base::ErrorCode code) {
    return base::Result<contracts::WorkerCommand>::failure(
        base::Error{code, "worker command JSON is invalid"});
}

std::uint64_t required_unsigned(const Json& object, const char* key) {
    const auto& value = object.at(key);
    if (!value.is_number_unsigned()) {
        throw std::invalid_argument("worker command field must be unsigned");
    }
    return value.get<std::uint64_t>();
}

Json encode_progress(const contracts::TaskProgress& progress) {
    return Json{
        {"schema_version", progress.schema_version},
        {"job_id", progress.job_id},
        {"trace_id", progress.trace_id},
        {"phase", static_cast<std::uint8_t>(progress.phase)},
        {"logical_bytes", progress.logical_bytes ? Json(*progress.logical_bytes) : Json(nullptr)},
        {"processed_bytes", progress.processed_bytes},
        {"stored_bytes", progress.stored_bytes},
        {"discovered_entries", progress.discovered_entries},
        {"processed_entries", progress.processed_entries},
        {"message_code", progress.message_code},
    };
}

Json encode_task_result(const contracts::TaskResult& result) {
    Json encoded{
        {"schema_version", result.schema_version},
        {"job_id", result.job_id},
        {"trace_id", result.trace_id},
        {"outcome", static_cast<std::uint8_t>(result.outcome)},
        {"error_code", static_cast<std::uint32_t>(result.error_code)},
        {"logical_bytes", result.logical_bytes},
        {"stored_bytes", result.stored_bytes},
        {"chunk_count", result.chunk_count},
        {"entry_count", result.entry_count},
        {"stream_count", result.stream_count},
        {"deduplicated_block_count", result.deduplicated_block_count},
        {"deduplicated_logical_bytes", result.deduplicated_logical_bytes},
        {"message_code", result.message_code},
        {"warning_codes", result.warning_codes},
        {"partial_restore", nullptr},
        {"requested_backup_type", nullptr},
        {"effective_backup_type", nullptr},
        {"effective_parent_uuid", nullptr},
        {"incremental_downgrade_reason", nullptr},
    };
    if (result.partial_restore) {
        encoded["partial_restore"] =
            Json{{"entries_requested", result.partial_restore->entries_requested},
                 {"entries_restored", result.partial_restore->entries_restored},
                 {"entries_failed", result.partial_restore->entries_failed},
                 {"bytes_restored", result.partial_restore->bytes_restored},
                 {"stable_error_codes", result.partial_restore->stable_error_codes}};
    }
    if (result.requested_backup_type) {
        encoded["requested_backup_type"] = *result.requested_backup_type;
    }
    if (result.effective_backup_type) {
        encoded["effective_backup_type"] = *result.effective_backup_type;
    }
    if (result.effective_parent_uuid) {
        encoded["effective_parent_uuid"] = *result.effective_parent_uuid;
    }
    if (result.incremental_downgrade_reason) {
        encoded["incremental_downgrade_reason"] =
            static_cast<std::uint8_t>(*result.incremental_downgrade_reason);
    }
    return encoded;
}

Json encode_response(const contracts::WorkerResponse& response) {
    Json encoded{
        {"schema_version", response.schema_version},
        {"job_id", response.job_id},
        {"trace_id", response.trace_id},
        {"kind", static_cast<std::uint8_t>(response.kind)},
        {"boundary_error_code", static_cast<std::uint32_t>(response.boundary_error_code)},
        {"message_code", response.message_code},
        {"task_result", nullptr},
    };
    if (response.task_result) {
        encoded["task_result"] = encode_task_result(*response.task_result);
    }
    return encoded;
}

} // namespace

base::Result<contracts::WorkerCommand> decode_worker_command(const std::string_view encoded) {
    if (encoded.empty() || encoded.size() > kMaximumCommandBytes) {
        return invalid_command(base::ErrorCode::kInvalidArgument);
    }
    try {
        const auto root = Json::parse(encoded);
        if (!root.is_object()) {
            return invalid_command(base::ErrorCode::kInvalidArgument);
        }
        const auto schema = required_unsigned(root, "schema_version");
        const auto kind = required_unsigned(root, "kind");
        if (schema > (std::numeric_limits<std::uint32_t>::max)() ||
            kind > (std::numeric_limits<std::uint8_t>::max)()) {
            return invalid_command(base::ErrorCode::kInvalidArgument);
        }
        contracts::WorkerCommand command;
        command.schema_version = static_cast<std::uint32_t>(schema);
        command.job_id = root.at("job_id").get<std::string>();
        command.trace_id = root.at("trace_id").get<std::string>();
        command.kind = static_cast<contracts::WorkerCommandKind>(kind);
        auto validation = contracts::validate_worker_command(command);
        if (!validation) {
            return base::Result<contracts::WorkerCommand>::failure(validation.error());
        }
        return base::Result<contracts::WorkerCommand>::success(std::move(command));
    } catch (const std::exception&) {
        return invalid_command(base::ErrorCode::kInvalidArgument);
    }
}

base::Result<std::string> encode_worker_event(const contracts::WorkerEvent& event) {
    auto validation = contracts::validate_worker_event(event);
    if (!validation) {
        return base::Result<std::string>::failure(validation.error());
    }
    try {
        Json root{
            {"schema_version", event.schema_version},
            {"job_id", event.job_id},
            {"trace_id", event.trace_id},
            {"kind", static_cast<std::uint8_t>(event.kind)},
            {"progress", nullptr},
            {"response", nullptr},
        };
        if (event.progress) {
            root["progress"] = encode_progress(*event.progress);
        } else if (event.response) {
            root["response"] = encode_response(*event.response);
        }
        return base::Result<std::string>::success(root.dump());
    } catch (const std::exception&) {
        return base::Result<std::string>::failure(
            base::Error{base::ErrorCode::kInternal, "worker event encoding failed"});
    }
}

} // namespace aegra::apps::worker
