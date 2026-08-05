#include "supervisor_worker_protocol.h"

#include "aegra/base/error.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/progress.h"
#include "aegra/contracts/task_result.h"
#include "aegra/contracts/worker_response.h"
#include "aegra/contracts/worker_session.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::apps::service {

using Json = nlohmann::json;

[[nodiscard]] base::Result<std::string>
encode_supervisor_job_request(const contracts::JobRequest& request) {
    if (auto valid = contracts::validate_job_request(request); !valid) {
        return base::Result<std::string>::failure(valid.error());
    }

    try {
        Json root = {{"schema_version", request.schema_version},
                     {"job_id", request.job_id},
                     {"tenant_id", request.tenant_id},
                     {"operation", static_cast<std::uint8_t>(request.operation)},
                     {"source_refs", request.source_refs},
                     {"trace_id", request.trace_id},
                     {"deadline_utc_ms", request.deadline_utc_ms}};

        if (!request.target_ref.empty()) {
            root["target_ref"] = request.target_ref;
        }

        std::vector<std::string> creds;
        creds.reserve(request.credential_refs.size());
        for (const auto& cred : request.credential_refs) {
            creds.push_back(cred.value);
        }
        root["credential_refs"] = creds;

        if (request.backup.has_value()) {
            Json backup = {
                {"type", static_cast<std::uint8_t>(request.backup->type)},
            };
            if (!request.backup->parent_source_ref.empty()) {
                backup["parent_source_ref"] = request.backup->parent_source_ref;
            }
            if (!request.backup->parent_credential_ref.value.empty()) {
                backup["parent_credential_ref"] = request.backup->parent_credential_ref.value;
            }
            backup["file_uuid"] = request.backup->file_uuid;
            backup["backup_set_uuid"] = request.backup->backup_set_uuid;
            backup["created_utc_ms"] = request.backup->created_utc_ms;
            backup["exclude_page_and_hibernation_files"] =
                request.backup->exclude_page_and_hibernation_files;
            backup["encryption_enabled"] = request.backup->encryption_enabled;
            root["backup"] = backup;
        }

        return base::Result<std::string>::success(root.dump());
    } catch (const std::exception&) {
        return base::Result<std::string>::failure(
            base::Error{base::ErrorCode::kInternal, "failed to encode job request"});
    }
}

[[nodiscard]] base::Result<contracts::WorkerEvent>
decode_supervisor_worker_event(std::string_view json_text) {
    if (json_text.empty()) {
        return base::Result<contracts::WorkerEvent>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "worker event json is empty"});
    }

    try {
        const auto root = Json::parse(json_text);
        if (!root.is_object()) {
            return base::Result<contracts::WorkerEvent>::failure(
                base::Error{base::ErrorCode::kInvalidArgument, "worker event is not an object"});
        }

        contracts::WorkerEvent event;
        event.schema_version = root.at("schema_version").get<std::uint32_t>();
        event.job_id = root.at("job_id").get<std::string>();
        event.trace_id = root.at("trace_id").get<std::string>();
        event.kind = static_cast<contracts::WorkerEventKind>(root.at("kind").get<std::uint8_t>());

        if (root.contains("progress") && !root.at("progress").is_null()) {
            const auto& p = root.at("progress");
            contracts::TaskProgress progress;
            progress.schema_version = p.at("schema_version").get<std::uint32_t>();
            progress.job_id = p.at("job_id").get<std::string>();
            progress.trace_id = p.at("trace_id").get<std::string>();
            progress.phase = static_cast<contracts::TaskPhase>(p.at("phase").get<std::uint8_t>());
            progress.logical_bytes = p.at("logical_bytes").get<std::uint64_t>();
            progress.processed_bytes = p.at("processed_bytes").get<std::uint64_t>();
            progress.stored_bytes = p.at("stored_bytes").get<std::uint64_t>();
            progress.message_code = p.at("message_code").get<std::string>();
            event.progress = progress;
        }

        if (root.contains("response") && !root.at("response").is_null()) {
            const auto& r = root.at("response");
            contracts::WorkerResponse response;
            response.schema_version = r.at("schema_version").get<std::uint32_t>();
            response.job_id = r.at("job_id").get<std::string>();
            response.trace_id = r.at("trace_id").get<std::string>();
            response.kind =
                static_cast<contracts::WorkerResponseKind>(r.at("kind").get<std::uint8_t>());
            response.boundary_error_code =
                static_cast<base::ErrorCode>(r.at("boundary_error_code").get<std::uint32_t>());
            response.message_code = r.at("message_code").get<std::string>();

            if (r.contains("task_result") && !r.at("task_result").is_null()) {
                const auto& t = r.at("task_result");
                contracts::TaskResult tr;
                tr.schema_version = t.at("schema_version").get<std::uint32_t>();
                tr.job_id = t.at("job_id").get<std::string>();
                tr.trace_id = t.at("trace_id").get<std::string>();
                tr.outcome =
                    static_cast<contracts::TaskOutcome>(t.at("outcome").get<std::uint8_t>());
                tr.error_code =
                    static_cast<base::ErrorCode>(t.at("error_code").get<std::uint32_t>());
                tr.logical_bytes = t.at("logical_bytes").get<std::uint64_t>();
                tr.stored_bytes = t.at("stored_bytes").get<std::uint64_t>();
                tr.chunk_count = t.at("chunk_count").get<std::uint64_t>();
                tr.message_code = t.at("message_code").get<std::string>();
                tr.warning_codes = t.at("warning_codes").get<std::vector<std::string>>();
                response.task_result = tr;
            }
            event.response = response;
        }

        if (auto valid = contracts::validate_worker_event(event); !valid) {
            return base::Result<contracts::WorkerEvent>::failure(valid.error());
        }

        return base::Result<contracts::WorkerEvent>::success(std::move(event));
    } catch (const std::exception&) {
        return base::Result<contracts::WorkerEvent>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "failed to decode worker event"});
    }
}

[[nodiscard]] base::Result<std::string>
encode_supervisor_worker_command(const contracts::WorkerCommand& command) {
    if (auto valid = contracts::validate_worker_command(command); !valid) {
        return base::Result<std::string>::failure(valid.error());
    }

    try {
        Json root = {{"schema_version", command.schema_version},
                     {"job_id", command.job_id},
                     {"trace_id", command.trace_id},
                     {"kind", static_cast<std::uint8_t>(command.kind)}};
        return base::Result<std::string>::success(root.dump());
    } catch (const std::exception&) {
        return base::Result<std::string>::failure(
            base::Error{base::ErrorCode::kInternal, "failed to encode worker command"});
    }
}

} // namespace aegra::apps::service
