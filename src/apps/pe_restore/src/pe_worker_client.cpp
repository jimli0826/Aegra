#include "pe_worker_client.h"

#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/contracts/progress.h"
#include "aegra/contracts/worker_response.h"
#include "aegra/contracts/worker_session.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <utility>

namespace aegra::apps::pe_restore {
namespace {

using Json = nlohmann::json;

[[nodiscard]] base::Error client_error(const base::ErrorCode code, const char* message) {
    return {code, message};
}

[[nodiscard]] base::Result<std::string> generate_pipe_name(ports::IRandomSource& random,
                                                           base::CancellationToken cancellation) {
    std::array<std::byte, 16> bytes{};
    if (auto filled = random.fill(bytes, std::move(cancellation)); !filled) {
        return base::Result<std::string>::failure(filled.error());
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string name = "pe-restore-";
    name.reserve(name.size() + bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        name.push_back(kHex[value >> 4U]);
        name.push_back(kHex[value & 0x0FU]);
    }
    return base::Result<std::string>::success(std::move(name));
}

/// Wire shape mirrors the service supervisor codec (supervisor_worker_protocol.cpp);
/// the JSON schema is the cross-process contract, implemented per composition root.
[[nodiscard]] base::Result<std::string> encode_restore_job(const contracts::JobRequest& request) {
    if (auto valid = contracts::validate_job_request(request); !valid) {
        return base::Result<std::string>::failure(valid.error());
    }
    if (!request.restore.has_value()) {
        return base::Result<std::string>::failure(
            client_error(base::ErrorCode::kInvalidArgument, "restore options are required"));
    }
    try {
        Json root = {{"schema_version", request.schema_version},
                     {"job_id", request.job_id},
                     {"tenant_id", request.tenant_id},
                     {"operation", static_cast<std::uint8_t>(request.operation)},
                     {"content_kind", static_cast<std::uint8_t>(request.content_kind)},
                     {"source_refs", request.source_refs},
                     {"trace_id", request.trace_id},
                     {"deadline_utc_ms", request.deadline_utc_ms},
                     {"target_ref", request.target_ref}};
        std::vector<std::string> credentials;
        credentials.reserve(request.credential_refs.size());
        for (const auto& credential : request.credential_refs) {
            credentials.push_back(credential.value);
        }
        root["credential_refs"] = credentials;
        Json edits = Json::array();
        for (const auto& edit : request.restore->partition_layout_edits) {
            edits.push_back(Json{{"source_start_offset_bytes", edit.source_start_offset_bytes},
                                 {"target_start_offset_bytes", edit.target_start_offset_bytes},
                                 {"size_bytes", edit.size_bytes}});
        }
        root["restore"] =
            Json{{"disk_restore", request.restore->disk_restore},
                 {"source_disk_number", request.restore->source_disk_number},
                 {"source_volume_index", request.restore->source_volume_index},
                 {"bring_target_online", request.restore->bring_target_online},
                 {"preserve_disk_signature", request.restore->preserve_disk_signature},
                 {"auto_expand_last_partition", request.restore->auto_expand_last_partition},
                 {"partition_layout_edits", std::move(edits)},
                 {"volume_size_policy",
                  static_cast<std::uint8_t>(request.restore->volume_size_policy)},
                 {"shrink_plan_digest", request.restore->shrink_plan_digest},
                 {"source_chain_fingerprint", request.restore->source_chain_fingerprint}};
        return base::Result<std::string>::success(root.dump());
    } catch (const std::exception&) {
        return base::Result<std::string>::failure(
            client_error(base::ErrorCode::kInternal, "failed to encode restore job"));
    }
}

struct DecodedEvent final {
    contracts::WorkerEventKind kind{contracts::WorkerEventKind::kProgress};
    PeWorkerProgress progress;
    PeWorkerOutcome outcome;
    base::ErrorCode boundary_error{base::ErrorCode::kNone};
};

[[nodiscard]] base::Result<DecodedEvent> decode_worker_event(const std::string& json_text,
                                                             const contracts::JobRequest& job) {
    try {
        const auto root = Json::parse(json_text);
        DecodedEvent event;
        if (root.at("job_id").get<std::string>() != job.job_id ||
            root.at("trace_id").get<std::string>() != job.trace_id) {
            return base::Result<DecodedEvent>::failure(
                client_error(base::ErrorCode::kConflict, "worker event correlation mismatch"));
        }
        event.kind =
            static_cast<contracts::WorkerEventKind>(root.at("kind").get<std::uint8_t>());
        if (event.kind == contracts::WorkerEventKind::kProgress && root.contains("progress") &&
            !root.at("progress").is_null()) {
            const auto& progress = root.at("progress");
            event.progress.processed_bytes = progress.at("processed_bytes").get<std::uint64_t>();
            if (!progress.at("logical_bytes").is_null()) {
                event.progress.logical_bytes = progress.at("logical_bytes").get<std::uint64_t>();
            }
            event.progress.message_code = progress.at("message_code").get<std::string>();
        }
        if (event.kind == contracts::WorkerEventKind::kResult && root.contains("response") &&
            !root.at("response").is_null()) {
            const auto& response = root.at("response");
            event.boundary_error =
                static_cast<base::ErrorCode>(response.at("boundary_error_code").get<std::uint32_t>());
            event.outcome.message_code = response.at("message_code").get<std::string>();
            if (response.contains("task_result") && !response.at("task_result").is_null()) {
                const auto& task = response.at("task_result");
                event.outcome.outcome =
                    static_cast<contracts::TaskOutcome>(task.at("outcome").get<std::uint8_t>());
                event.outcome.error_code =
                    static_cast<base::ErrorCode>(task.at("error_code").get<std::uint32_t>());
                event.outcome.message_code = task.at("message_code").get<std::string>();
            } else {
                event.outcome.outcome = contracts::TaskOutcome::kFailed;
                event.outcome.error_code = event.boundary_error == base::ErrorCode::kNone
                                               ? base::ErrorCode::kInternal
                                               : event.boundary_error;
            }
        }
        return base::Result<DecodedEvent>::success(std::move(event));
    } catch (const std::exception&) {
        return base::Result<DecodedEvent>::failure(
            client_error(base::ErrorCode::kCorruptData, "failed to decode worker event"));
    }
}

[[nodiscard]] base::Result<PeWorkerOutcome>
receive_until_result(ports::IMessageChannel& channel, const PeWorkerSessionRequest& request,
                     const base::CancellationToken& cancellation) {
    for (;;) {
        auto received = channel.receive(cancellation);
        if (!received) {
            return base::Result<PeWorkerOutcome>::failure(received.error());
        }
        auto event = decode_worker_event(received.value(), request.job);
        if (!event) {
            return base::Result<PeWorkerOutcome>::failure(event.error());
        }
        if (event.value().kind == contracts::WorkerEventKind::kResult) {
            return base::Result<PeWorkerOutcome>::success(std::move(event).value().outcome);
        }
        if (event.value().kind == contracts::WorkerEventKind::kProgress && request.on_progress) {
            request.on_progress(event.value().progress);
        }
    }
}

} // namespace

base::Result<PeWorkerOutcome> run_pe_worker_session(const PeWorkerSessionRequest& request,
                                                    const base::CancellationToken cancellation) {
    using Output = base::Result<PeWorkerOutcome>;
    if (request.launcher == nullptr || request.random == nullptr ||
        request.worker_executable_path_utf8.empty()) {
        return Output::failure(
            client_error(base::ErrorCode::kInvalidArgument, "worker session request is invalid"));
    }
    auto pipe_name = generate_pipe_name(*request.random, cancellation);
    if (!pipe_name) {
        return Output::failure(pipe_name.error());
    }
    auto encoded = encode_restore_job(request.job);
    if (!encoded) {
        return Output::failure(encoded.error());
    }
    auto listener = adapters::windows_ipc::WindowsNamedPipeListener::create(
        {pipe_name.value(), 1024U * 1024U,
         adapters::windows_ipc::WindowsNamedPipeAclProfile::kProcessDefault,
         adapters::windows_ipc::WindowsNamedPipeNamespace::kWorker});
    if (!listener) {
        return Output::failure(listener.error());
    }
    auto launched = request.launcher->launch(
        {request.worker_executable_path_utf8, {"--pipe", pipe_name.value()}, false});
    if (!launched) {
        return Output::failure(launched.error());
    }
    const auto worker_pid = launched.value().pid;
    auto channel = listener.value()->accept(cancellation);
    if (!channel) {
        (void)request.launcher->terminate(worker_pid);
        (void)request.launcher->wait(worker_pid, {});
        return Output::failure(channel.error());
    }
    if (auto sent = channel.value()->send(encoded.value(), cancellation); !sent) {
        (void)request.launcher->terminate(worker_pid);
        (void)request.launcher->wait(worker_pid, {});
        return Output::failure(sent.error());
    }
    auto outcome = receive_until_result(*channel.value(), request, cancellation);
    // The worker exits on its own after sending the terminal result; always join it.
    (void)request.launcher->wait(worker_pid, {});
    return outcome;
}

} // namespace aegra::apps::pe_restore
