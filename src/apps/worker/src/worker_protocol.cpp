#include "aegra/apps/worker/worker_protocol.h"

#include "aegra/base/error.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/task_result.h"
#include "aegra/contracts/worker_response.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::worker {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaximumWorkerRequestBytes = std::size_t{1024} * 1024U;

base::Result<contracts::JobRequest> invalid_job(const base::ErrorCode code, const char* message) {
    return base::Result<contracts::JobRequest>::failure(base::Error{code, message});
}

template <typename T> T required(const Json& object, const char* key) {
    return object.at(key).get<T>();
}

std::uint64_t required_unsigned(const Json& object, const char* key) {
    const auto& value = object.at(key);
    if (!value.is_number_unsigned()) {
        throw std::invalid_argument("worker request field must be an unsigned integer");
    }
    return value.get<std::uint64_t>();
}

std::int64_t optional_deadline(const Json& object) {
    const auto iterator = object.find("deadline_utc_ms");
    if (iterator == object.end()) {
        return 0;
    }
    if (iterator->is_number_unsigned()) {
        const auto value = iterator->get<std::uint64_t>();
        if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(value);
        }
    } else if (iterator->is_number_integer()) {
        return iterator->get<std::int64_t>();
    }
    throw std::out_of_range("worker request deadline is out of range");
}

contracts::JobRequest parse_job(const Json& root) {
    const auto schema_version = required_unsigned(root, "schema_version");
    const auto operation = required_unsigned(root, "operation");
    if (schema_version > std::numeric_limits<std::uint32_t>::max() ||
        operation > std::numeric_limits<std::uint8_t>::max()) {
        throw std::out_of_range("worker request integer is out of range");
    }
    contracts::JobRequest job;
    job.schema_version = static_cast<std::uint32_t>(schema_version);
    job.job_id = required<std::string>(root, "job_id");
    job.tenant_id = required<std::string>(root, "tenant_id");
    job.operation = static_cast<contracts::JobOperation>(operation);
    job.source_refs = required<std::vector<std::string>>(root, "source_refs");
    const auto target = root.find("target_ref");
    if (target != root.end()) {
        if (!target->is_string()) {
            throw std::invalid_argument("worker request target_ref must be a string");
        }
        job.target_ref = target->get<std::string>();
    }
    job.trace_id = required<std::string>(root, "trace_id");
    job.deadline_utc_ms = optional_deadline(root);
    for (const auto& value : required<std::vector<std::string>>(root, "credential_refs")) {
        job.credential_refs.push_back(contracts::SecretRef{value});
    }
    return job;
}

Json encode_task_result(const contracts::TaskResult& result) {
    return Json{
        {"schema_version", result.schema_version},
        {"job_id", result.job_id},
        {"trace_id", result.trace_id},
        {"outcome", static_cast<std::uint8_t>(result.outcome)},
        {"error_code", static_cast<std::uint32_t>(result.error_code)},
        {"logical_bytes", result.logical_bytes},
        {"stored_bytes", result.stored_bytes},
        {"chunk_count", result.chunk_count},
        {"message_code", result.message_code},
        {"warning_codes", result.warning_codes},
    };
}

Json encode_response_object(const contracts::WorkerResponse& response) {
    Json root{
        {"schema_version", response.schema_version},
        {"job_id", response.job_id},
        {"trace_id", response.trace_id},
        {"kind", static_cast<std::uint8_t>(response.kind)},
        {"boundary_error_code", static_cast<std::uint32_t>(response.boundary_error_code)},
        {"message_code", response.message_code},
    };
    if (response.task_result) {
        root["task_result"] = encode_task_result(*response.task_result);
    } else {
        root["task_result"] = nullptr;
    }
    return root;
}

contracts::WorkerResponse rejection_response(const base::ErrorCode code) {
    contracts::WorkerResponse response;
    response.kind = contracts::WorkerResponseKind::kRequestRejected;
    response.boundary_error_code = code;
    response.message_code = "worker.request_rejected";
    return response;
}

} // namespace

base::Result<contracts::JobRequest> decode_worker_job_request(const std::string_view encoded) {
    if (encoded.empty()) {
        return invalid_job(base::ErrorCode::kInvalidArgument, "worker request is empty");
    }
    if (encoded.size() > kMaximumWorkerRequestBytes) {
        return invalid_job(base::ErrorCode::kInvalidArgument, "worker request is too large");
    }
    try {
        const auto root = Json::parse(encoded);
        if (!root.is_object()) {
            return invalid_job(base::ErrorCode::kInvalidArgument,
                               "worker request root must be an object");
        }
        if (root.contains("password") || root.contains("secret")) {
            return invalid_job(base::ErrorCode::kInvalidArgument,
                               "plaintext credential fields are forbidden");
        }
        auto job = parse_job(root);
        auto validation = contracts::validate_job_request(job);
        if (!validation) {
            return base::Result<contracts::JobRequest>::failure(validation.error());
        }
        return base::Result<contracts::JobRequest>::success(std::move(job));
    } catch (const std::exception&) {
        return invalid_job(base::ErrorCode::kInvalidArgument, "worker request JSON is invalid");
    }
}

base::Result<std::string> encode_worker_response(const contracts::WorkerResponse& response) {
    auto validation = contracts::validate_worker_response(response);
    if (!validation) {
        return base::Result<std::string>::failure(validation.error());
    }
    try {
        return base::Result<std::string>::success(encode_response_object(response).dump());
    } catch (const std::exception&) {
        return base::Result<std::string>::failure(base::Error{
            base::ErrorCode::kInternal,
            "worker response encoding failed",
        });
    }
}

base::Result<EncodedWorkerResult> run_windows_personal_backup_worker_request(
    const std::string_view encoded_request, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context, const base::CancellationToken& cancellation) {
    auto job = decode_worker_job_request(encoded_request);
    if (!job) {
        auto response = encode_worker_response(rejection_response(job.error().code));
        if (!response) {
            return base::Result<EncodedWorkerResult>::failure(response.error());
        }
        return base::Result<EncodedWorkerResult>::success(
            EncodedWorkerResult{WorkerExitCode::kRequestRejected, std::move(response).value()});
    }
    auto result =
        run_windows_personal_backup_worker_host(job.value(), options, context, cancellation);
    auto encoded_response = encode_worker_response(result.response);
    if (!encoded_response) {
        return base::Result<EncodedWorkerResult>::failure(encoded_response.error());
    }
    return base::Result<EncodedWorkerResult>::success(
        EncodedWorkerResult{result.exit_code, std::move(encoded_response).value()});
}

} // namespace aegra::apps::worker
