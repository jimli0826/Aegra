#include "client/service_client.h"

#include "client/service_protocol.h"
#include "locale/message_code_map.h"

#include <QDateTime>
#include <QJsonObject>
#include <QUuid>

namespace aegra::desktop {
namespace {

constexpr qsizetype kMaximumSources = 10'000;
constexpr qsizetype kMaximumConnections = 1'000;

[[nodiscard]] void fill_pending_from_schedule(const QVariantList& schedules,
                                              const QString& schedule_id, QStringList& source_ids,
                                              QString& connection_id) {
    source_ids.clear();
    connection_id.clear();
    for (const auto& item : schedules) {
        const auto map = item.toMap();
        const auto id = map.value(QStringLiteral("scheduleId")).toString();
        if (id != schedule_id && map.value(QStringLiteral("id")).toString() != schedule_id) {
            continue;
        }
        connection_id = map.value(QStringLiteral("connectionId")).toString();
        // volume_set: JobSummary.source_ids are volume identity codes.
        for (const auto& source : map.value(QStringLiteral("sourceIds")).toList()) {
            const auto source_id = source.toString();
            if (!source_id.isEmpty()) {
                source_ids.push_back(source_id);
            }
        }
        // file_set: schedule.source_ids is empty; Service puts selection_id on the job's
        // source_ids field so Desktop can correlate schedule rows to job status.
        if (source_ids.isEmpty()) {
            for (const auto& summary : map.value(QStringLiteral("selectionSummaries")).toList()) {
                const auto selection_id =
                    summary.toMap().value(QStringLiteral("selectionId")).toString();
                if (!selection_id.isEmpty()) {
                    source_ids.push_back(selection_id);
                }
            }
        }
        return;
    }
}

[[nodiscard]] QString job_state_translation(const std::int64_t state) {
    switch (state) {
    case 1:
        //% "Queued"
        return qtTrId("aegra.task.state.queued");
    case 2:
        //% "Running"
        return qtTrId("aegra.task.state.running");
    case 3:
        //% "Cancelling"
        return qtTrId("aegra.task.state.cancelling");
    case 4:
        //% "Succeeded"
        return qtTrId("aegra.task.state.succeeded");
    case 5:
        //% "Failed"
        return qtTrId("aegra.task.state.failed");
    case 6:
        //% "Cancelled"
        return qtTrId("aegra.task.state.cancelled");
    case 7:
        //% "Interrupted"
        return qtTrId("aegra.task.state.interrupted");
    default:
        //% "Unknown"
        return qtTrId("aegra.common.unknown");
    }
}

[[nodiscard]] int progress_percent_of(const JobRow& row) noexcept {
    if (!row.progress_logical_bytes || !row.progress_processed_bytes) {
        return 0;
    }
    const auto logical = *row.progress_logical_bytes;
    const auto processed = *row.progress_processed_bytes;
    if (logical <= 0 || processed < 0 || processed > logical) {
        return 0;
    }
    if (processed == logical) {
        return 100;
    }
    return static_cast<int>((processed * 100) / logical);
}

} // namespace

QString ServiceClient::defaultConnectionId() const { return connections_.default_connection_id(); }

bool ServiceClient::startBackup(const QString& schedule_id, const int backup_type) {
    if (state_ != State::kReady) {
        //% "Service is not connected"
        const auto msg = qtTrId("aegra.error.service.disconnected");
        emit backupStartFailed(msg);
        show_toast(msg);
        return false;
    }
    if (!backup_start_available_) {
        //% "Service does not support backup.start"
        const auto msg = qtTrId("aegra.backup.run.capability_missing");
        emit backupStartFailed(msg);
        show_toast(msg);
        return false;
    }
    if (schedule_id.isEmpty() ||
        (backup_type != kBackupTypeFull && backup_type != kBackupTypeIncremental)) {
        finish_backup_command_failure(QStringLiteral("backup.preflight_failed"));
        return false;
    }
    if (backup_command_busy_ || cancel_command_busy_) {
        //% "A backup command is already in progress"
        const auto msg = qtTrId("aegra.backup.run.busy");
        emit backupStartFailed(msg);
        show_toast(msg);
        return false;
    }
    // Do not start a second backup while the observed job is still active.
    if (!active_backup_job_id_.isEmpty() && !active_backup_terminal_) {
        //% "A backup job is already running"
        const auto msg = qtTrId("aegra.backup.run.already_running");
        emit backupStartFailed(msg);
        show_toast(msg);
        return false;
    }
    // Fresh attempt after terminal or first click: mint a new idempotency key.
    // Mid-flight retry (key set, no job yet) reuses the key so Service can replay.
    if (start_backup_idempotency_key_.isEmpty() || active_backup_terminal_) {
        if (active_backup_terminal_) {
            active_backup_job_id_.clear();
        }
        start_backup_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    backup_command_error_code_.clear();
    backup_command_busy_ = true;
    pending_backup_schedule_id_ = schedule_id;
    fill_pending_from_schedule(schedules_, schedule_id, pending_backup_source_ids_,
                               pending_backup_connection_id_);
    emit backupCommandChanged();
    update_active_backup_observe();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    start_backup_request_id_ = request_id;
    const auto body = encode_start_backup_request(request_id, start_backup_idempotency_key_,
                                                  schedule_id, backup_type);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_start_backup_frame(frame_body);
        });
    if (!started) {
        finish_backup_command_failure(QStringLiteral("service.send_failed"));
        return false;
    }
    return true;
}

void ServiceClient::cancelActiveBackup() {
    if (state_ != State::kReady || !job_cancel_available_ || cancel_command_busy_ ||
        backup_command_busy_ || active_backup_job_id_.isEmpty() || !active_backup_cancellable_) {
        return;
    }
    if (cancel_job_idempotency_key_.isEmpty()) {
        cancel_job_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    backup_command_error_code_.clear();
    cancel_command_busy_ = true;
    emit backupCommandChanged();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    cancel_job_request_id_ = request_id;
    const auto body =
        encode_cancel_job_request(request_id, cancel_job_idempotency_key_, active_backup_job_id_);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_cancel_job_frame(frame_body);
        });
    if (!started) {
        finish_cancel_command_failure(QStringLiteral("service.send_failed"));
    }
}

void ServiceClient::start_inventory_query() {
    if (!inventory_available_) {
        return;
    }
    // Do not clear the live sources model until a successful response replaces it —
    // clearing/pending-only would make Home flash empty / "Loading sources…".
    inventory_error_code_.clear();
    inventory_loading_ = true;
    pending_sources_.clear();
    inventory_requested_token_.reset();
    emit inventoryChanged();
    emit loadingChanged();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    inventory_request_id_ = request_id;
    const auto body = encode_source_inventory_request(request_id, std::nullopt, true);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_inventory_frame(frame_body);
        });
    if (!started) {
        finish_inventory_failure(QStringLiteral("inventory.query_failed"));
    }
}

void ServiceClient::start_connection_query() {
    if (!connections_available_) {
        return;
    }
    connections_error_code_.clear();
    connections_loading_ = true;
    pending_connections_.clear();
    connection_requested_token_.reset();
    emit connectionsChanged();
    emit loadingChanged();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    connection_request_id_ = request_id;
    const auto body = encode_repository_connection_list_request(request_id, std::nullopt);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_connection_list_frame(frame_body);
        });
    if (!started) {
        finish_connection_failure(QStringLiteral("connection.query_failed"));
    }
}

RequestDisposition ServiceClient::handle_inventory_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_inventory_failure_response(root)) {
        finish_inventory_failure(QStringLiteral("inventory.query_failed"));
        return RequestDisposition::kFinished;
    }
    SourceInventoryPage page;
    if (!parse_source_inventory_response(root, page)) {
        return RequestDisposition::kProtocolError;
    }
    if ((page.continuation_token && page.continuation_token == inventory_requested_token_) ||
        pending_sources_.size() + page.items.size() > kMaximumSources) {
        return RequestDisposition::kProtocolError;
    }
    QSet<QString> seen_ids;
    for (const auto& existing : pending_sources_) {
        seen_ids.insert(existing.toMap().value(QStringLiteral("sourceId")).toString());
    }
    for (auto& item : page.items) {
        const auto source_id = item.toMap().value(QStringLiteral("sourceId")).toString();
        if (seen_ids.contains(source_id)) {
            return RequestDisposition::kProtocolError;
        }
        seen_ids.insert(source_id);
        pending_sources_.push_back(std::move(item));
    }
    if (page.continuation_token) {
        inventory_requested_token_ = page.continuation_token;
        const auto next_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto next_body =
            encode_source_inventory_request(next_id, inventory_requested_token_, true);
        if (!coordinator_->continue_request(request_id, next_id, next_body)) {
            return RequestDisposition::kProtocolError;
        }
        inventory_request_id_ = next_id;
        return RequestDisposition::kContinue;
    }
    sources_.set_rows(sources_from_variant_list(pending_sources_));
    pending_sources_.clear();
    inventory_loading_ = false;
    inventory_request_id_.clear();
    inventory_requested_token_.reset();
    emit inventoryChanged();
    emit loadingChanged();
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_connection_list_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_connection_list_failure_response(root)) {
        finish_connection_failure(QStringLiteral("connection.query_failed"));
        return RequestDisposition::kFinished;
    }
    RepositoryConnectionPage page;
    if (!parse_repository_connection_list_response(root, page)) {
        return RequestDisposition::kProtocolError;
    }
    if ((page.continuation_token && page.continuation_token == connection_requested_token_) ||
        pending_connections_.size() + page.items.size() > kMaximumConnections) {
        return RequestDisposition::kProtocolError;
    }
    QSet<QString> seen_ids;
    for (const auto& existing : pending_connections_) {
        seen_ids.insert(existing.toMap().value(QStringLiteral("connectionId")).toString());
    }
    for (auto& item : page.items) {
        const auto connection_id = item.toMap().value(QStringLiteral("connectionId")).toString();
        if (seen_ids.contains(connection_id)) {
            return RequestDisposition::kProtocolError;
        }
        seen_ids.insert(connection_id);
        pending_connections_.push_back(std::move(item));
    }
    if (page.continuation_token) {
        connection_requested_token_ = page.continuation_token;
        const auto next_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto next_body =
            encode_repository_connection_list_request(next_id, connection_requested_token_);
        if (!coordinator_->continue_request(request_id, next_id, next_body)) {
            return RequestDisposition::kProtocolError;
        }
        connection_request_id_ = next_id;
        return RequestDisposition::kContinue;
    }
    connections_.set_rows(connections_from_variant_list(pending_connections_));
    pending_connections_.clear();
    connections_loading_ = false;
    connection_request_id_.clear();
    connection_requested_token_.reset();
    if (!selected_repository_connection_id_.isEmpty() &&
        !connections_.find(selected_repository_connection_id_)) {
        selected_repository_connection_id_.clear();
    }
    if (selected_repository_connection_id_.isEmpty()) {
        selected_repository_connection_id_ = connections_.default_connection_id();
    }
    emit connectionsChanged();
    emit loadingChanged();
    if (!schedules_.isEmpty()) {
        enrich_schedules_with_connections();
        schedule_list_.set_items(schedules_);
        emit schedulesChanged();
    }
    if (selected_repository_connection_id_.isEmpty()) {
        reset_repository();
    }
    if (repository_refresh_waiting_for_snapshot_) {
        repository_refresh_waiting_for_snapshot_ = false;
        repository_refresh_current_id_.clear();
        begin_next_repository_refresh();
    }
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_start_backup_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, kStartBackupRequestKind)) {
        const auto code = root.value(QStringLiteral("message_code")).toString();
        finish_backup_command_failure(code.isEmpty() ? QStringLiteral("backup.command_failed")
                                                     : code);
        return RequestDisposition::kFinished;
    }
    CommandAck ack;
    if (!parse_command_ack_response(root, kStartBackupRequestKind, ack) || !ack.has_resource_id ||
        ack.resource_id.isEmpty()) {
        return RequestDisposition::kProtocolError;
    }
    active_backup_job_id_ = ack.resource_id;
    backup_command_busy_ = false;
    start_backup_request_id_.clear();
    // Keep idempotency key until the job terminates so reconnect/replay cannot create a second job.
    backup_command_error_code_.clear();
    cancel_job_idempotency_key_.clear();
    // Optimistic task row so Home Tasks updates immediately (before job.list returns).
    {
        JobRow optimistic;
        optimistic.job_id = active_backup_job_id_;
        optimistic.operation = 1; // backup
        optimistic.state = 1;      // queued
        optimistic.created_utc_ms = QDateTime::currentMSecsSinceEpoch();
        optimistic.source_ids = pending_backup_source_ids_;
        optimistic.schedule_id = pending_backup_schedule_id_;
        optimistic.connection_id = pending_backup_connection_id_;
        enrich_job_row(optimistic);
        jobs_.upsert_job(std::move(optimistic));
        emit jobsChanged();
    }
    emit backupCommandChanged();
    update_active_backup_observe();
    //% "Backup started"
    show_toast(qtTrId("aegra.backup.run.started"));
    emit backupStartSucceeded(active_backup_job_id_);
    if (job_list_available_ && !jobs_loading_) {
        start_job_query();
    }
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_cancel_job_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, kCancelJobRequestKind)) {
        const auto code = root.value(QStringLiteral("message_code")).toString();
        finish_cancel_command_failure(code.isEmpty() ? QStringLiteral("job.cancel_failed") : code);
        return RequestDisposition::kFinished;
    }
    CommandAck ack;
    if (!parse_command_ack_response(root, kCancelJobRequestKind, ack)) {
        return RequestDisposition::kProtocolError;
    }
    cancel_command_busy_ = false;
    cancel_job_request_id_.clear();
    backup_command_error_code_.clear();
    emit backupCommandChanged();
    if (job_list_available_ && !jobs_loading_) {
        start_job_query();
    }
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_inventory_failure(const QString& message_code) {
    pending_sources_.clear();
    inventory_requested_token_.reset();
    inventory_loading_ = false;
    inventory_request_id_.clear();
    inventory_error_code_ = message_code;
    // Preserve the last complete Service snapshot across transient refresh failures.
    emit inventoryChanged();
    emit loadingChanged();
}

void ServiceClient::finish_connection_failure(const QString& message_code) {
    pending_connections_.clear();
    connection_requested_token_.reset();
    connections_loading_ = false;
    connection_request_id_.clear();
    connections_error_code_ = message_code;
    if (repository_refresh_waiting_for_snapshot_) {
        stop_repository_refresh();
    }
    // Preserve registered repositories while the Service reconnects.
    emit connectionsChanged();
    emit loadingChanged();
}

void ServiceClient::finish_backup_command_failure(const QString& message_code) {
    backup_command_busy_ = false;
    start_backup_request_id_.clear();
    start_backup_idempotency_key_.clear();
    backup_command_error_code_ = message_code;
    emit backupCommandChanged();
    update_active_backup_observe();
    const auto msg = localize_message_code(message_code);
    show_toast(msg, true);
    emit backupStartFailed(msg);
}

void ServiceClient::finish_cancel_command_failure(const QString& message_code) {
    cancel_command_busy_ = false;
    cancel_job_request_id_.clear();
    cancel_job_idempotency_key_.clear();
    backup_command_error_code_ = message_code;
    emit backupCommandChanged();
}

void ServiceClient::reset_inventory() {
    sources_.clear();
    pending_sources_.clear();
    inventory_requested_token_.reset();
    inventory_loading_ = false;
    inventory_error_code_.clear();
    inventory_request_id_.clear();
    emit inventoryChanged();
}

void ServiceClient::reset_connections() {
    stop_repository_refresh();
    connections_.clear();
    pending_connections_.clear();
    connection_requested_token_.reset();
    connections_loading_ = false;
    connections_error_code_.clear();
    connection_request_id_.clear();
    selected_repository_connection_id_.clear();
    emit connectionsChanged();
}

void ServiceClient::reset_backup_command() {
    // Drop in-flight command bookkeeping on disconnect, but keep the observed job id so
    // reconnect + job.list can resume progress/cancel without presenting a blank page.
    backup_command_busy_ = false;
    cancel_command_busy_ = false;
    start_backup_request_id_.clear();
    cancel_job_request_id_.clear();
    backup_command_error_code_.clear();
    emit backupCommandChanged();
    update_active_backup_observe();
}

void ServiceClient::enrich_job_row(JobRow& row) const {
    if (!row.source_ids.isEmpty()) {
        QStringList source_names;
        source_names.reserve(row.source_ids.size());
        for (const auto& source_id : row.source_ids) {
            if (const auto source = sources_.find(source_id)) {
                source_names.push_back(source->display_name);
            } else {
                source_names.push_back(source_id);
            }
        }
        row.source_name = source_names.join(QStringLiteral(", "));
    }
    if (!row.connection_id.isEmpty()) {
        if (const auto connection = connections_.find(row.connection_id)) {
            row.destination_name = connection->display_name;
            // Connection summary currently exposes display name only (no path locator).
            if (row.destination_path.isEmpty()) {
                row.destination_path = connection->display_name;
            }
        } else if (row.destination_name.isEmpty()) {
            row.destination_name = row.connection_id;
        }
    }
}

void ServiceClient::update_active_backup_observe() {
    QString state_text;
    QString message_text;
    int percent = 0;
    bool progress_visible = false;
    bool terminal = false;
    bool cancellable = false;
    if (!active_backup_job_id_.isEmpty()) {
        if (const auto job = jobs_.find_job(active_backup_job_id_)) {
            state_text = job_state_translation(job->state);
            message_text =
                job->message_code.isEmpty() ? QString{} : localize_message_code(job->message_code);
            percent = job->state == 4 ? 100 : progress_percent_of(*job);
            progress_visible = job->state == 4 || job->state == 1 || job->state == 2 ||
                               job->state == 3 || job->progress_processed_bytes.has_value();
            terminal = job->state == 4 || job->state == 5 || job->state == 6 || job->state == 7;
            cancellable = job_cancel_available_ && (job->state == 1 || job->state == 2) &&
                          !cancel_command_busy_ && !backup_command_busy_;
            if (terminal) {
                start_backup_idempotency_key_.clear();
                cancel_job_idempotency_key_.clear();
            }
        } else if (backup_command_busy_) {
            //% "Submitting"
            state_text = qtTrId("aegra.backup.state.submitting");
        } else {
            //% "Waiting for job status"
            state_text = qtTrId("aegra.backup.state.waiting_status");
        }
    }
    const auto changed =
        active_backup_state_text_ != state_text || active_backup_message_text_ != message_text ||
        active_backup_progress_percent_ != percent ||
        active_backup_progress_visible_ != progress_visible ||
        active_backup_terminal_ != terminal || active_backup_cancellable_ != cancellable;
    active_backup_state_text_ = std::move(state_text);
    active_backup_message_text_ = std::move(message_text);
    active_backup_progress_percent_ = percent;
    active_backup_progress_visible_ = progress_visible;
    active_backup_terminal_ = terminal;
    active_backup_cancellable_ = cancellable;
    if (changed) {
        emit backupObserveChanged();
        emit backupCommandChanged();
    }
}

} // namespace aegra::desktop
