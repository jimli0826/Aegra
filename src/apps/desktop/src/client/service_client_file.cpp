#include "client/service_client.h"

#include "client/service_protocol.h"
#include "locale/message_code_map.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <utility>

namespace aegra::desktop {
namespace {

[[nodiscard]] QString parse_failure_message_code(const QByteArray& body) {
    const auto document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return QStringLiteral("service.request_failed");
    }
    const auto code = document.object().value(QStringLiteral("message_code")).toString();
    return code.isEmpty() ? QStringLiteral("service.request_failed") : code;
}

[[nodiscard]] QVector<FileBrowseNode> nodes_from_page(const FileBrowsePage& page,
                                                      const int depth,
                                                      const QString& parent_token) {
    QVector<FileBrowseNode> nodes;
    nodes.reserve(page.items.size());
    for (const auto& item : page.items) {
        nodes.push_back(file_browse_node_from_map(item.toMap(), depth, parent_token));
    }
    return nodes;
}

[[nodiscard]] QVector<FileRecoverNode> recover_nodes_from_page(const RecoveryPointEntryPage& page,
                                                               const int depth,
                                                               const QString& parent_entry_id) {
    QVector<FileRecoverNode> nodes;
    nodes.reserve(page.items.size());
    for (const auto& item : page.items) {
        const auto map = item.toMap();
        FileRecoverNode node;
        node.entry_id = map.value(QStringLiteral("entryId")).toString();
        node.parent_entry_id = parent_entry_id;
        node.display_name = map.value(QStringLiteral("displayName")).toString();
        node.entry_kind = map.value(QStringLiteral("entryKind")).toLongLong();
        node.logical_size_bytes =
            static_cast<std::uint64_t>(map.value(QStringLiteral("logicalSizeBytes")).toULongLong());
        node.has_children = map.value(QStringLiteral("hasChildren")).toBool();
        node.message_code = map.value(QStringLiteral("messageCode")).toString();
        node.depth = depth;
        nodes.push_back(std::move(node));
    }
    return nodes;
}

} // namespace

bool ServiceClient::fileBrowseAvailable() const noexcept { return file_browse_available_; }

bool ServiceClient::fileRestoreAvailable() const noexcept { return file_restore_available_; }

bool ServiceClient::fileRecoverBrowseAvailable() const noexcept {
    return file_recover_browse_available_;
}

FileBrowseModel* ServiceClient::fileBrowseSources() noexcept { return &file_browse_sources_; }

FileBrowseModel* ServiceClient::fileRestoreTargets() noexcept { return &file_restore_targets_; }

FileRecoverModel* ServiceClient::fileRecoverEntries() noexcept { return &file_recover_entries_; }

void ServiceClient::loadFileBrowseRoots() {
    if (state_ != State::kReady || !file_browse_available_) {
        return;
    }
    file_browse_sources_.setSingleDirectoryMode(false);
    file_browse_sources_.clear();
    file_browse_sources_.set_loading(true);
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    file_browse_request_id_ = request_id;
    file_browse_parent_token_.clear();
    const auto body = encode_browse_file_sources_request(request_id, std::nullopt);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_browse_file_sources_frame(frame_body);
        });
    if (!started) {
        file_browse_sources_.set_loading(false);
        file_browse_sources_.set_error_text(localize_message_code(QStringLiteral("file_browse.query_failed")));
    }
}

void ServiceClient::clearFileRestoreState() {
    reset_file_models();
    restore_preflight_token_.clear();
    restore_prepare_only_ = false;
    restore_recovery_point_id_.clear();
    restore_archive_password_.clear();
    restore_start_idempotency_key_.clear();
    restore_prepare_request_id_.clear();
    restore_start_request_id_.clear();
}

void ServiceClient::loadFileRestoreTargetRoots() {
    if (state_ != State::kReady || !file_browse_available_) {
        return;
    }
    file_restore_targets_.setSingleDirectoryMode(true);
    file_restore_targets_.clear();
    file_restore_targets_.set_loading(true);
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    file_target_browse_request_id_ = request_id;
    file_target_browse_parent_token_.clear();
    const auto body = encode_browse_file_sources_request(request_id, std::nullopt);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_file_target_browse_frame(frame_body);
        });
    if (!started) {
        file_restore_targets_.set_loading(false);
        file_restore_targets_.set_error_text(
            localize_message_code(QStringLiteral("file_browse.query_failed")));
    }
}

void ServiceClient::on_file_browse_expand_requested(const QString& node_token) {
    if (state_ != State::kReady || !file_browse_available_ || node_token.isEmpty()) {
        return;
    }
    file_browse_sources_.set_node_loading(node_token, true);
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    file_browse_request_id_ = request_id;
    file_browse_parent_token_ = node_token;
    const auto body = encode_browse_file_sources_request(request_id, node_token);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_browse_file_sources_frame(frame_body);
        });
    if (!started) {
        file_browse_sources_.set_node_loading(node_token, false);
        file_browse_sources_.set_error_text(
            localize_message_code(QStringLiteral("file_browse.query_failed")));
    }
}

void ServiceClient::on_file_target_expand_requested(const QString& node_token) {
    if (state_ != State::kReady || !file_browse_available_ || node_token.isEmpty()) {
        return;
    }
    file_restore_targets_.set_node_loading(node_token, true);
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    file_target_browse_request_id_ = request_id;
    file_target_browse_parent_token_ = node_token;
    const auto body = encode_browse_file_sources_request(request_id, node_token);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_file_target_browse_frame(frame_body);
        });
    if (!started) {
        file_restore_targets_.set_node_loading(node_token, false);
        file_restore_targets_.set_error_text(
            localize_message_code(QStringLiteral("file_browse.query_failed")));
    }
}

void ServiceClient::loadFileRecoverRoots(const QString& recovery_point_id,
                                         const QString& archive_password) {
    if (state_ != State::kReady || !file_recover_browse_available_ || recovery_point_id.isEmpty()) {
        return;
    }
    const auto connection_id = selected_repository_connection_id_.isEmpty()
                                   ? defaultConnectionId()
                                   : selected_repository_connection_id_;
    // Always drop prior selection/tree so re-opening the same checkpoint starts clean.
    file_recover_entries_.clear();
    file_recover_entries_.set_context(recovery_point_id, connection_id);
    file_recover_entries_.set_loading(true);
    file_recover_archive_password_ = archive_password;
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    file_recover_request_id_ = request_id;
    file_recover_parent_entry_id_ = QStringLiteral("0");
    const auto body = encode_list_recovery_point_entries_request(
        request_id, connection_id, recovery_point_id, QStringLiteral("0"), std::nullopt,
        archive_password);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_list_recovery_point_entries_frame(frame_body);
        });
    if (!started) {
        file_recover_entries_.set_loading(false);
        file_recover_entries_.set_error_text(
            localize_message_code(QStringLiteral("file_recover.query_failed")));
    }
}

void ServiceClient::on_file_recover_expand_requested(const QString& entry_id) {
    if (state_ != State::kReady || !file_recover_browse_available_ || entry_id.isEmpty() ||
        file_recover_entries_.recoveryPointId().isEmpty()) {
        return;
    }
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    file_recover_request_id_ = request_id;
    file_recover_parent_entry_id_ = entry_id;
    const auto body = encode_list_recovery_point_entries_request(
        request_id, file_recover_entries_.connectionId(), file_recover_entries_.recoveryPointId(),
        entry_id, std::nullopt, file_recover_archive_password_);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_list_recovery_point_entries_frame(frame_body);
        });
    if (!started) {
        file_recover_entries_.set_error_text(
            localize_message_code(QStringLiteral("file_recover.query_failed")));
    }
}

bool ServiceClient::createFileSetSchedule(const QString& connection_id, const QString& frequency,
                                          const QString& time_of_day,
                                          const bool exclude_page_and_hibernation_files,
                                          const bool encryption_enabled,
                                          const QString& archive_password,
                                          const bool start_full_backup_after_create,
                                          const int backup_type) {
    if (state_ != State::kReady || !schedules_available_ || !file_browse_available_ ||
        schedule_command_busy_ || connection_id.isEmpty()) {
        return false;
    }
    // file_set: Full or Incremental only (never Differential).
    if (backup_type != kBackupTypeFull && backup_type != kBackupTypeIncremental) {
        return false;
    }
    const auto selections = file_browse_sources_.selected_file_selections();
    if (selections.isEmpty() || selections.size() > 100) {
        return false;
    }
    if (encryption_enabled &&
        (archive_password.isEmpty() || archive_password.size() > 32)) {
        return false;
    }
    if (!encryption_enabled && !archive_password.isEmpty()) {
        return false;
    }
    QStringList labels;
    for (const auto& item : selections) {
        labels.push_back(item.toMap().value(QStringLiteral("displayLabel")).toString());
    }
    const auto display_name = labels.join(QStringLiteral(", "));
    const auto trigger_kind =
        frequency.compare(QStringLiteral("weekly"), Qt::CaseInsensitive) == 0
            ? kScheduleTriggerWeekly
            : kScheduleTriggerDaily;
    int local_minute = 2 * 60;
    {
        const auto parts = time_of_day.split(QLatin1Char(':'));
        if (parts.size() == 2) {
            bool ok_h = false;
            bool ok_m = false;
            const int hour = parts[0].toInt(&ok_h);
            const int minute = parts[1].toInt(&ok_m);
            if (ok_h && ok_m && hour >= 0 && hour < 24 && minute >= 0 && minute < 60) {
                local_minute = hour * 60 + minute;
            }
        }
    }
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto idempotency_key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    schedule_command_request_id_ = request_id;
    schedule_command_idempotency_key_ = idempotency_key;
    schedule_command_kind_ = kUpsertScheduleRequestKind;
    schedule_command_busy_ = true;
    start_full_backup_after_schedule_create_ = start_full_backup_after_create;
    const auto body = encode_upsert_file_set_schedule_request(
        request_id, idempotency_key, {}, display_name, true, selections, connection_id,
        backup_type, trigger_kind, local_minute, 0, QStringLiteral("UTC"),
        exclude_page_and_hibernation_files, encryption_enabled, archive_password);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_schedule_command_frame(frame_body);
        });
    if (!started) {
        start_full_backup_after_schedule_create_ = false;
        finish_schedule_command_failure(QStringLiteral("schedule.command_failed"));
        return false;
    }
    return true;
}

bool ServiceClient::prepareFileRestore(const QString& recovery_point_id, const int conflict_policy,
                                       const QString& archive_password,
                                       const bool restore_security) {
    if (state_ != State::kReady || !file_restore_available_ || restore_command_busy_ ||
        recovery_point_id.isEmpty()) {
        return false;
    }
    const auto entry_ids = file_recover_entries_.selected_entry_ids();
    const auto target_token = file_restore_targets_.selected_directory_token();
    if (entry_ids.isEmpty() || target_token.isEmpty()) {
        //% "Select files to restore and a target folder"
        show_toast(qtTrId("aegra.restore.file.selection_required"), true);
        return false;
    }
    const auto connection_id = selected_repository_connection_id_.isEmpty()
                                   ? defaultConnectionId()
                                   : selected_repository_connection_id_;
    restore_command_busy_ = true;
    restore_prepare_only_ = true;
    restore_recovery_point_id_ = recovery_point_id;
    restore_archive_password_ = archive_password;
    restore_preflight_token_.clear();
    restore_start_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    file_restore_entry_ids_ = entry_ids;
    file_restore_target_token_ = target_token;
    file_restore_conflict_policy_ = conflict_policy;
    file_restore_security_ = restore_security;
    emit restoreCommandChanged();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    restore_prepare_request_id_ = request_id;
    const auto body = encode_prepare_file_restore_request(
        request_id, connection_id, recovery_point_id, entry_ids, target_token, conflict_policy,
        archive_password, restore_security);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_prepare_file_restore_frame(frame_body);
        });
    if (!started) {
        finish_restore_preflight_failure(QStringLiteral("file_restore.preflight_failed"));
        return false;
    }
    return true;
}

bool ServiceClient::startPreparedFileRestore() {
    if (state_ != State::kReady || !file_restore_available_ || restore_command_busy_ ||
        restore_preflight_token_.isEmpty()) {
        return false;
    }
    restore_command_busy_ = true;
    restore_prepare_only_ = false;
    if (restore_start_idempotency_key_.isEmpty()) {
        restore_start_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    emit restoreCommandChanged();

    const auto start_request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    restore_start_request_id_ = start_request_id;
    const auto start_body = encode_start_file_restore_request(
        start_request_id, restore_start_idempotency_key_, restore_preflight_token_,
        restore_archive_password_);
    const auto started = coordinator_->begin_request(
        start_request_id, start_body, [this](const QByteArray& frame_body) {
            return handle_start_file_restore_frame(frame_body);
        });
    if (!started) {
        finish_restore_command_failure(QStringLiteral("file_restore.command_failed"));
        return false;
    }
    return true;
}

bool ServiceClient::startFileRestore(const QString& recovery_point_id, const int conflict_policy,
                                     const QString& archive_password,
                                     const bool restore_security) {
    if (state_ != State::kReady || !file_restore_available_ || restore_command_busy_ ||
        recovery_point_id.isEmpty()) {
        return false;
    }
    // Reuse a successful prepare when selection/options match.
    if (!restore_preflight_token_.isEmpty() && restore_recovery_point_id_ == recovery_point_id &&
        file_restore_conflict_policy_ == conflict_policy &&
        file_restore_security_ == restore_security &&
        restore_archive_password_ == archive_password) {
        const auto entry_ids = file_recover_entries_.selected_entry_ids();
        const auto target_token = file_restore_targets_.selected_directory_token();
        if (entry_ids == file_restore_entry_ids_ && target_token == file_restore_target_token_) {
            return startPreparedFileRestore();
        }
    }
    const auto entry_ids = file_recover_entries_.selected_entry_ids();
    const auto target_token = file_restore_targets_.selected_directory_token();
    if (entry_ids.isEmpty() || target_token.isEmpty()) {
        //% "Select files to restore and a target folder"
        show_toast(qtTrId("aegra.restore.file.selection_required"), true);
        return false;
    }
    const auto connection_id = selected_repository_connection_id_.isEmpty()
                                   ? defaultConnectionId()
                                   : selected_repository_connection_id_;
    restore_command_busy_ = true;
    restore_prepare_only_ = false;
    restore_recovery_point_id_ = recovery_point_id;
    restore_archive_password_ = archive_password;
    restore_preflight_token_.clear();
    restore_start_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    file_restore_entry_ids_ = entry_ids;
    file_restore_target_token_ = target_token;
    file_restore_conflict_policy_ = conflict_policy;
    file_restore_security_ = restore_security;
    emit restoreCommandChanged();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    restore_prepare_request_id_ = request_id;
    const auto body = encode_prepare_file_restore_request(
        request_id, connection_id, recovery_point_id, entry_ids, target_token, conflict_policy,
        archive_password, restore_security);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_prepare_file_restore_frame(frame_body);
        });
    if (!started) {
        finish_restore_command_failure(QStringLiteral("file_restore.preflight_failed"));
        return false;
    }
    return true;
}

RequestDisposition ServiceClient::handle_browse_file_sources_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_browse_file_sources_failure_response(root)) {
        file_browse_sources_.set_loading(false);
        if (!file_browse_parent_token_.isEmpty()) {
            file_browse_sources_.set_node_loading(file_browse_parent_token_, false);
        }
        file_browse_sources_.set_error_text(
            localize_message_code(parse_failure_message_code(body)));
        return RequestDisposition::kFinished;
    }
    FileBrowsePage page;
    if (!parse_browse_file_sources_response(root, page)) {
        return RequestDisposition::kProtocolError;
    }
    if (file_browse_parent_token_.isEmpty()) {
        file_browse_sources_.set_roots(nodes_from_page(page, 0, {}));
    } else {
        file_browse_sources_.set_children(file_browse_parent_token_,
                                          nodes_from_page(page, 0, file_browse_parent_token_));
    }
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_file_target_browse_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_browse_file_sources_failure_response(root)) {
        file_restore_targets_.set_loading(false);
        if (!file_target_browse_parent_token_.isEmpty()) {
            file_restore_targets_.set_node_loading(file_target_browse_parent_token_, false);
        }
        file_restore_targets_.set_error_text(
            localize_message_code(parse_failure_message_code(body)));
        return RequestDisposition::kFinished;
    }
    FileBrowsePage page;
    if (!parse_browse_file_sources_response(root, page)) {
        return RequestDisposition::kProtocolError;
    }
    if (file_target_browse_parent_token_.isEmpty()) {
        file_restore_targets_.set_roots(nodes_from_page(page, 0, {}));
    } else {
        file_restore_targets_.set_children(
            file_target_browse_parent_token_,
            nodes_from_page(page, 0, file_target_browse_parent_token_));
    }
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_list_recovery_point_entries_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_list_recovery_point_entries_failure_response(root)) {
        file_recover_entries_.set_loading(false);
        file_recover_entries_.set_error_text(
            localize_message_code(parse_failure_message_code(body)));
        return RequestDisposition::kFinished;
    }
    RecoveryPointEntryPage page;
    if (!parse_list_recovery_point_entries_response(root, page)) {
        return RequestDisposition::kProtocolError;
    }
    auto nodes =
        recover_nodes_from_page(page, 0, file_recover_parent_entry_id_);
    if (file_recover_parent_entry_id_ == QStringLiteral("0")) {
        file_recover_entries_.set_roots(std::move(nodes), page.index_generation);
    } else {
        file_recover_entries_.set_children(file_recover_parent_entry_id_, std::move(nodes),
                                           page.index_generation);
    }
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_prepare_file_restore_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    const bool prepare_only = restore_prepare_only_;
    if (is_prepare_file_restore_failure_response(root)) {
        if (prepare_only) {
            finish_restore_preflight_failure(parse_failure_message_code(body));
        } else {
            finish_restore_command_failure(parse_failure_message_code(body));
        }
        return RequestDisposition::kFinished;
    }
    FileRestorePreflightPage preflight;
    if (!parse_prepare_file_restore_response(root, preflight) || !preflight.restore_eligible) {
        const auto code = preflight.message_code.isEmpty()
                              ? QStringLiteral("file_restore.preflight_failed")
                              : preflight.message_code;
        if (prepare_only) {
            finish_restore_preflight_failure(code);
        } else {
            finish_restore_command_failure(code);
        }
        return RequestDisposition::kFinished;
    }
    restore_preflight_token_ = preflight.preflight_token;
    if (prepare_only) {
        restore_prepare_only_ = false;
        restore_command_busy_ = false;
        emit restoreCommandChanged();
        emit restorePreflightSucceeded();
        return RequestDisposition::kFinished;
    }
    const auto start_request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    restore_start_request_id_ = start_request_id;
    const auto start_body = encode_start_file_restore_request(
        start_request_id, restore_start_idempotency_key_, restore_preflight_token_,
        restore_archive_password_);
    const auto started = coordinator_->begin_request(
        start_request_id, start_body, [this](const QByteArray& frame_body) {
            return handle_start_file_restore_frame(frame_body);
        });
    if (!started) {
        finish_restore_command_failure(QStringLiteral("file_restore.command_failed"));
    }
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_start_file_restore_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    CommandAck ack;
    if (is_command_failure_response(root, kStartFileRestoreRequestKind)) {
        finish_restore_command_failure(parse_failure_message_code(body));
        return RequestDisposition::kFinished;
    }
    if (!parse_command_ack_response(root, kStartFileRestoreRequestKind, ack)) {
        return RequestDisposition::kProtocolError;
    }
    restore_command_busy_ = false;
    emit restoreCommandChanged();
    emit restoreStartSucceeded();
    //% "File restore started"
    show_toast(qtTrId("aegra.restore.file.started"));
    if (job_list_available_) {
        start_job_query();
    }
    return RequestDisposition::kFinished;
}

} // namespace aegra::desktop
