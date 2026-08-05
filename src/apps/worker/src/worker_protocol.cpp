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

std::optional<contracts::BackupOptions> optional_backup(const Json& root) {
    const auto iterator = root.find("backup");
    if (iterator == root.end()) {
        return std::nullopt;
    }
    if (!iterator->is_object()) {
        throw std::invalid_argument("worker request backup must be an object");
    }
    const auto type = required_unsigned(*iterator, "type");
    if (type > std::numeric_limits<std::uint8_t>::max()) {
        throw std::out_of_range("worker request backup type is out of range");
    }
    contracts::BackupOptions result;
    result.type = static_cast<contracts::BackupType>(type);
    result.parent_source_ref = iterator->value("parent_source_ref", std::string{});
    result.parent_credential_ref.value =
        iterator->value("parent_credential_ref", std::string{});
    result.file_uuid = required<std::string>(*iterator, "file_uuid");
    result.backup_set_uuid = required<std::string>(*iterator, "backup_set_uuid");
    result.created_utc_ms = required<std::int64_t>(*iterator, "created_utc_ms");
    // Required field — no default for omitted keys (unreleased; no wire compatibility path).
    const auto exclude = iterator->find("exclude_page_and_hibernation_files");
    if (exclude == iterator->end() || !exclude->is_boolean()) {
        throw std::invalid_argument(
            "worker request backup.exclude_page_and_hibernation_files is required");
    }
    result.exclude_page_and_hibernation_files = exclude->get<bool>();
    return result;
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
    job.backup = optional_backup(root);
    return job;
}

bool contains_plaintext_credential_field(const Json& object) {
    if (!object.is_object()) {
        return false;
    }
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
        if (iterator.key().find("password") != std::string::npos ||
            iterator.key().find("secret") != std::string::npos) {
            return true;
        }
    }
    return false;
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
        if (contains_plaintext_credential_field(root)) {
            return invalid_job(base::ErrorCode::kInvalidArgument,
                               "plaintext credential fields are forbidden");
        }
        const auto backup = root.find("backup");
        if (backup != root.end() && contains_plaintext_credential_field(*backup)) {
            return invalid_job(base::ErrorCode::kInvalidArgument,
                               "plaintext backup credential fields are forbidden");
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
