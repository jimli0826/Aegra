#include "boot_check_protocol.h"

#include "aegra/contracts/task_result.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace aegra::apps::boot_check::detail {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumRequestBytes = std::size_t{1024} * 1024U;

base::Result<contracts::BootCheckJobRequest> invalid_request(const char* message) {
    return base::Result<contracts::BootCheckJobRequest>::failure(
        {base::ErrorCode::kInvalidArgument, message});
}

[[nodiscard]] bool key_names_plaintext_credential(const std::string& key) {
    std::string lowered(key);
    std::ranges::transform(lowered, lowered.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return lowered.find("password") != std::string::npos ||
           lowered.find("secret") != std::string::npos;
}

[[nodiscard]] bool contains_plaintext_credential_field(const Json& value) {
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (key_names_plaintext_credential(key) || contains_plaintext_credential_field(child)) {
                return true;
            }
        }
        return false;
    }
    if (value.is_array()) {
        return std::any_of(value.begin(), value.end(), [](const Json& child) {
            return contains_plaintext_credential_field(child);
        });
    }
    return false;
}

[[nodiscard]] std::uint64_t required_unsigned(const Json& object, const char* key) {
    const auto& value = object.at(key);
    if (!value.is_number_unsigned()) {
        throw std::invalid_argument("boot check request field must be an unsigned integer");
    }
    return value.get<std::uint64_t>();
}

[[nodiscard]] contracts::BootCheckJobRequest parse_request(const Json& root) {
    contracts::BootCheckJobRequest request;
    const auto schema = required_unsigned(root, "schema_version");
    if (schema > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::out_of_range("boot check request schema version is out of range");
    }
    request.schema_version = static_cast<std::uint32_t>(schema);
    request.job_id = root.at("job_id").get<std::string>();
    request.trace_id = root.at("trace_id").get<std::string>();
    const auto hypervisor = required_unsigned(root, "hypervisor");
    if (hypervisor > (std::numeric_limits<std::uint8_t>::max)()) {
        throw std::out_of_range("boot check hypervisor is out of range");
    }
    request.hypervisor =
        static_cast<contracts::BootCheckHypervisor>(static_cast<std::uint8_t>(hypervisor));
    const auto cpu_count = required_unsigned(root, "cpu_count");
    const auto memory_mib = required_unsigned(root, "memory_mib");
    if (cpu_count > (std::numeric_limits<std::uint32_t>::max)() ||
        memory_mib > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::out_of_range("boot check VM resources are out of range");
    }
    request.cpu_count = static_cast<std::uint32_t>(cpu_count);
    request.memory_mib = static_cast<std::uint32_t>(memory_mib);
    request.source_refs = root.at("source_refs").get<std::vector<std::string>>();
    for (const auto& reference : root.at("credential_refs").get<std::vector<std::string>>()) {
        request.credential_refs.push_back(contracts::SecretRef{reference});
    }
    request.job_directory = root.at("job_directory").get<std::string>();
    const auto deadline = root.find("deadline_utc_ms");
    if (deadline != root.end() && !deadline->is_null()) {
        request.deadline_utc_ms = required_unsigned(root, "deadline_utc_ms");
    }
    return request;
}

[[nodiscard]] Json encode_task_result(const contracts::TaskResult& result) {
    // BootCheck never reports partial-restore or backup-type fields; the shared
    // wire shape keeps them explicitly null so the Service decoder is unchanged.
    return Json{
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
}

} // namespace

base::Result<contracts::BootCheckJobRequest>
decode_boot_check_job_request(const std::string_view encoded) {
    if (encoded.empty()) {
        return invalid_request("boot check request is empty");
    }
    if (encoded.size() > kMaximumRequestBytes) {
        return invalid_request("boot check request is too large");
    }
    try {
        const auto root = Json::parse(encoded);
        if (!root.is_object()) {
            return invalid_request("boot check request root must be an object");
        }
        constexpr std::array<std::string_view, 10> keys{
            "schema_version", "job_id",       "trace_id",      "hypervisor",
            "cpu_count",      "memory_mib",    "source_refs",   "credential_refs",
            "job_directory",  "deadline_utc_ms"};
        if (root.size() != keys.size() ||
            !std::ranges::all_of(keys, [&root](const auto key) { return root.contains(key); })) {
            return invalid_request("boot check request fields are invalid");
        }
        if (contains_plaintext_credential_field(root)) {
            return invalid_request("plaintext credential fields are forbidden");
        }
        auto request = parse_request(root);
        if (request.schema_version != contracts::kBootCheckJobSchemaVersion) {
            return base::Result<contracts::BootCheckJobRequest>::failure(
                {base::ErrorCode::kUnsupportedVersion,
                 "unsupported boot check job schema version"});
        }
        return base::Result<contracts::BootCheckJobRequest>::success(std::move(request));
    } catch (const std::exception&) {
        return invalid_request("boot check request is malformed");
    }
}

base::Result<std::string> encode_boot_check_response(const contracts::WorkerResponse& response) {
    try {
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
        return base::Result<std::string>::success(root.dump());
    } catch (const std::exception&) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInternal, "boot check response encoding failed"});
    }
}

} // namespace aegra::apps::boot_check::detail
