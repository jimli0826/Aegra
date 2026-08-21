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
                     {"content_kind", static_cast<std::uint8_t>(request.content_kind)},
                     {"source_refs", request.source_refs},
                     {"trace_id", request.trace_id},
                     {"deadline_utc_ms", request.deadline_utc_ms}};

        // file_set wire payloads are added when F5/F6 start file jobs. Volume jobs keep empty
        // file_source_refs / null file_restore_target.
        if (!request.file_source_refs.empty()) {
            Json file_refs = Json::array();
            for (const auto& ref : request.file_source_refs) {
                Json components = Json::array();
                for (const auto& name : ref.relative_components) {
                    std::vector<std::uint8_t> bytes;
                    bytes.reserve(name.bytes.size());
                    for (const auto item : name.bytes) {
                        bytes.push_back(static_cast<std::uint8_t>(item));
                    }
                    components.push_back(
                        Json{{"encoding", static_cast<std::uint8_t>(name.encoding)},
                             {"bytes", std::move(bytes)}});
                }
                file_refs.push_back(
                    Json{{"selection_id", ref.selection_id},
                         {"volume_identity", ref.volume_identity},
                         {"relative_components", std::move(components)},
                         {"entry_kind", static_cast<std::uint8_t>(ref.entry_kind)},
                         {"recursion", static_cast<std::uint8_t>(ref.recursion)},
                         {"unreadable_policy", static_cast<std::uint8_t>(ref.unreadable_policy)},
                         {"display_label", ref.display_label}});
            }
            root["file_source_refs"] = std::move(file_refs);
        }
        if (request.file_restore_target) {
            root["file_restore_target"] =
                Json{{"target_root_identity", request.file_restore_target->target_root_identity},
                     {"entry_ids", request.file_restore_target->entry_ids},
                     {"conflict_policy",
                      static_cast<std::uint8_t>(request.file_restore_target->conflict_policy)},
                     {"restore_security", request.file_restore_target->restore_security}};
        }

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
            backup["deduplication_enabled"] = request.backup->deduplication_enabled;
            if (!request.backup->candidate_parent_uuid.empty()) {
                backup["candidate_parent_uuid"] = request.backup->candidate_parent_uuid;
            }
            if (request.backup->selection_fingerprint) {
                std::vector<std::uint8_t> digest;
                digest.reserve(request.backup->selection_fingerprint->digest.size());
                for (const auto item : request.backup->selection_fingerprint->digest) {
                    digest.push_back(static_cast<std::uint8_t>(item));
                }
                backup["selection_fingerprint"] =
                    Json{{"algorithm_id", request.backup->selection_fingerprint->algorithm_id},
                         {"digest", std::move(digest)}};
            } else {
                backup["selection_fingerprint"] = nullptr;
            }
            if (request.backup->service_full_reason) {
                backup["service_full_reason"] =
                    static_cast<std::uint8_t>(*request.backup->service_full_reason);
            }
            root["backup"] = backup;
        }

        if (request.restore.has_value()) {
            Json edits = Json::array();
            for (const auto& edit : request.restore->partition_layout_edits) {
                edits.push_back(
                    Json{{"source_start_offset_bytes", edit.source_start_offset_bytes},
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
            if (!p.at("logical_bytes").is_null()) {
                progress.logical_bytes = p.at("logical_bytes").get<std::uint64_t>();
            }
            progress.processed_bytes = p.at("processed_bytes").get<std::uint64_t>();
            progress.stored_bytes = p.at("stored_bytes").get<std::uint64_t>();
            if (p.contains("discovered_entries") && !p.at("discovered_entries").is_null()) {
                progress.discovered_entries = p.at("discovered_entries").get<std::uint64_t>();
            }
            if (p.contains("processed_entries") && !p.at("processed_entries").is_null()) {
                progress.processed_entries = p.at("processed_entries").get<std::uint64_t>();
            }
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
                if (t.contains("entry_count")) {
                    tr.entry_count = t.at("entry_count").get<std::uint64_t>();
                }
                if (t.contains("stream_count")) {
                    tr.stream_count = t.at("stream_count").get<std::uint64_t>();
                }
                // Worker schema 4 requires explicit ADR-0022 metrics (no default/fallback).
                tr.deduplicated_block_count = t.at("deduplicated_block_count").get<std::uint64_t>();
                tr.deduplicated_logical_bytes =
                    t.at("deduplicated_logical_bytes").get<std::uint64_t>();
                tr.message_code = t.at("message_code").get<std::string>();
                tr.warning_codes = t.at("warning_codes").get<std::vector<std::string>>();
                if (t.contains("partial_restore") && !t.at("partial_restore").is_null()) {
                    const auto& partial = t.at("partial_restore");
                    contracts::PartialRestoreStats stats;
                    stats.entries_requested = partial.at("entries_requested").get<std::uint64_t>();
                    stats.entries_restored = partial.at("entries_restored").get<std::uint64_t>();
                    stats.entries_failed = partial.at("entries_failed").get<std::uint64_t>();
                    stats.bytes_restored = partial.at("bytes_restored").get<std::uint64_t>();
                    stats.stable_error_codes =
                        partial.at("stable_error_codes").get<std::vector<std::string>>();
                    tr.partial_restore = std::move(stats);
                }
                if (t.contains("requested_backup_type") && !t.at("requested_backup_type").is_null()) {
                    tr.requested_backup_type = t.at("requested_backup_type").get<std::uint8_t>();
                }
                if (t.contains("effective_backup_type") && !t.at("effective_backup_type").is_null()) {
                    tr.effective_backup_type = t.at("effective_backup_type").get<std::uint8_t>();
                }
                if (t.contains("effective_parent_uuid") && !t.at("effective_parent_uuid").is_null()) {
                    tr.effective_parent_uuid = t.at("effective_parent_uuid").get<std::string>();
                }
                if (t.contains("incremental_downgrade_reason") &&
                    !t.at("incremental_downgrade_reason").is_null()) {
                    tr.incremental_downgrade_reason =
                        static_cast<contracts::IncrementalDowngradeReason>(
                            t.at("incremental_downgrade_reason").get<std::uint8_t>());
                }
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
