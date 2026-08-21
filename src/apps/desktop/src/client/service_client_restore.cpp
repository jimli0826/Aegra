#include "client/service_client.h"

#include "client/service_protocol.h"
#include "locale/message_code_map.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QVariantMap>

namespace aegra::desktop {
namespace {

[[nodiscard]] QString parse_failure_message_code(const QByteArray& body) {
    const auto document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return QStringLiteral("service.request_failed");
    }
    const auto root = document.object();
    const auto code = root.value(QStringLiteral("message_code")).toString();
    return code.isEmpty() ? QStringLiteral("service.request_failed") : code;
}

[[nodiscard]] bool is_command_accepted(const QByteArray& body) {
    const auto document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return false;
    }
    const auto kind = document.object().value(QStringLiteral("kind")).toInt();
    return kind == kCommandAcceptedResponseKind;
}

[[nodiscard]] QVariantMap restore_preflight_details(const RestorePreflightPage& preflight) {
    return QVariantMap{
        {QStringLiteral("preflightToken"), preflight.preflight_token},
        {QStringLiteral("feasibility"), preflight.feasibility},
        {QStringLiteral("restoreEligible"), preflight.restore_eligible},
        {QStringLiteral("logicalSizeBytes"),
         static_cast<qulonglong>(preflight.logical_size_bytes)},
        {QStringLiteral("targetCapacityBytes"),
         static_cast<qulonglong>(preflight.target_capacity_bytes)},
        {QStringLiteral("minimumTargetBytes"),
         static_cast<qulonglong>(preflight.minimum_target_bytes)},
        {QStringLiteral("relocationBytes"), static_cast<qulonglong>(preflight.relocation_bytes)},
        {QStringLiteral("scratchUpperBoundBytes"),
         static_cast<qulonglong>(preflight.scratch_upper_bound_bytes)},
        {QStringLiteral("shrinkPlanDigest"), preflight.shrink_plan_digest},
        {QStringLiteral("messageCode"), preflight.message_code},
        {QStringLiteral("restrictionCodes"), preflight.restriction_codes},
        {QStringLiteral("warningCodes"), preflight.warning_codes},
        {QStringLiteral("targetSourceId"), preflight.target_source_id},
        {QStringLiteral("recoveryPointId"), preflight.recovery_point_id},
    };
}

} // namespace

bool ServiceClient::begin_volume_prepare_or_analyze(const int source_volume_index,
                                                    const QString& target_source_id,
                                                    const QString& recovery_point_id,
                                                    const QString& archive_password,
                                                    const bool analyze) {
    if (state_ != State::kReady) {
        //% "Service is not connected"
        show_toast(qtTrId("aegra.error.service.disconnected"), true);
        return false;
    }
    if (!restore_start_available_ || !restore_preflight_available_ ||
        (analyze && !ntfs_shrink_available_)) {
        //% "Service does not support restore"
        show_toast(qtTrId("aegra.restore.capability_missing"), true);
        return false;
    }
    if (restore_command_busy_) {
        //% "A restore command is already in progress"
        show_toast(qtTrId("aegra.restore.busy"), true);
        return false;
    }
    if (source_volume_index < 0 || target_source_id.isEmpty() || recovery_point_id.isEmpty() ||
        !target_source_id.startsWith(QStringLiteral("vol."))) {
        //% "Select a checkpoint and map a source volume to a target volume"
        show_toast(qtTrId("aegra.restore.volume_map_required"), true);
        return false;
    }
    const auto connection_id = defaultConnectionId();
    if (connection_id.isEmpty()) {
        //% "No repository connection is available"
        show_toast(qtTrId("aegra.restore.no_repository"), true);
        return false;
    }

    restore_command_busy_ = true;
    restore_source_disk_number_ = -1;
    restore_target_disk_number_ = -1;
    restore_source_volume_index_ = source_volume_index;
    restore_target_source_id_ = target_source_id;
    restore_preserve_disk_signature_ = true;
    restore_auto_expand_last_partition_ = true;
    restore_partition_layout_edits_.clear();
    restore_recovery_point_id_ = recovery_point_id;
    restore_archive_password_ = archive_password;
    restore_preflight_token_.clear();
    restore_start_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit restoreCommandChanged();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    restore_prepare_request_id_ = request_id;
    const auto body =
        analyze ? encode_analyze_ntfs_shrink_request(request_id, connection_id, recovery_point_id,
                                                     target_source_id, 0, source_volume_index,
                                                     archive_password)
                : encode_prepare_restore_request(request_id, connection_id, recovery_point_id,
                                                 target_source_id, 0, source_volume_index,
                                                 archive_password,
                                                 kVolumeSizePolicyRequireSourceSize);
    const auto started = coordinator_->begin_request(
        request_id, body, [this, analyze](const QByteArray& frame_body) {
            return analyze ? handle_analyze_ntfs_shrink_frame(frame_body)
                           : handle_prepare_restore_frame(frame_body);
        });
    if (!started) {
        finish_restore_command_failure(QStringLiteral("service.send_failed"));
        return false;
    }
    return true;
}

bool ServiceClient::startDiskRestore(const int source_disk_number, const int target_disk_number,
                                     const QString& recovery_point_id,
                                     const QString& archive_password,
                                     const bool preserve_disk_signature,
                                     const bool auto_expand_last_partition,
                                     const QVariantList& partition_layout_edits) {
    if (state_ != State::kReady) {
        //% "Service is not connected"
        show_toast(qtTrId("aegra.error.service.disconnected"), true);
        return false;
    }
    if (!restore_start_available_ || !restore_preflight_available_) {
        //% "Service does not support restore"
        show_toast(qtTrId("aegra.restore.capability_missing"), true);
        return false;
    }
    if (restore_command_busy_) {
        //% "A restore command is already in progress"
        show_toast(qtTrId("aegra.restore.busy"), true);
        return false;
    }
    if (source_disk_number < 0 || target_disk_number < 0 || recovery_point_id.isEmpty()) {
        //% "Select a checkpoint and map a source disk to a target disk"
        show_toast(qtTrId("aegra.restore.map_required"), true);
        return false;
    }
    const auto connection_id = defaultConnectionId();
    if (connection_id.isEmpty()) {
        //% "No repository connection is available"
        show_toast(qtTrId("aegra.restore.no_repository"), true);
        return false;
    }

    restore_command_busy_ = true;
    restore_source_disk_number_ = source_disk_number;
    restore_target_disk_number_ = target_disk_number;
    restore_source_volume_index_ = -1;
    restore_target_source_id_.clear();
    restore_preserve_disk_signature_ = preserve_disk_signature;
    restore_auto_expand_last_partition_ = auto_expand_last_partition;
    restore_partition_layout_edits_ = partition_layout_edits;
    restore_recovery_point_id_ = recovery_point_id;
    restore_archive_password_ = archive_password;
    restore_preflight_token_.clear();
    restore_start_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit restoreCommandChanged();

    const auto target_source_id = QStringLiteral("disk.%1").arg(target_disk_number);
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    restore_prepare_request_id_ = request_id;
    const auto body =
        encode_prepare_restore_request(request_id, connection_id, recovery_point_id,
                                       target_source_id, source_disk_number, 0, archive_password);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_prepare_restore_frame(frame_body);
        });
    if (!started) {
        finish_restore_command_failure(QStringLiteral("service.send_failed"));
        return false;
    }
    return true;
}

bool ServiceClient::startVolumeRestore(const int source_volume_index,
                                       const QString& target_source_id,
                                       const QString& recovery_point_id,
                                       const QString& archive_password) {
    return begin_volume_prepare_or_analyze(source_volume_index, target_source_id, recovery_point_id,
                                           archive_password, false);
}

bool ServiceClient::analyzeVolumeShrink(const int source_volume_index,
                                        const QString& target_source_id,
                                        const QString& recovery_point_id,
                                        const QString& archive_password) {
    return begin_volume_prepare_or_analyze(source_volume_index, target_source_id, recovery_point_id,
                                           archive_password, true);
}

bool ServiceClient::startRestoreWithPreflightToken(const QString& preflight_token,
                                                   const QString& archive_password) {
    if (state_ != State::kReady) {
        //% "Service is not connected"
        show_toast(qtTrId("aegra.error.service.disconnected"), true);
        return false;
    }
    if (!restore_start_available_ || preflight_token.isEmpty()) {
        //% "Service does not support restore"
        show_toast(qtTrId("aegra.restore.capability_missing"), true);
        return false;
    }
    if (restore_command_busy_) {
        //% "A restore command is already in progress"
        show_toast(qtTrId("aegra.restore.busy"), true);
        return false;
    }
    restore_archive_password_ = archive_password;
    restore_preflight_token_ = preflight_token;
    restore_command_busy_ = true;
    restore_start_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit restoreCommandChanged();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    restore_start_request_id_ = request_id;
    const auto start_body = encode_start_restore_request(
        request_id, restore_start_idempotency_key_, restore_preflight_token_,
        restore_archive_password_, restore_preserve_disk_signature_,
        restore_auto_expand_last_partition_, restore_partition_layout_edits_);
    const auto started =
        coordinator_->begin_request(request_id, start_body, [this](const QByteArray& frame_body) {
            return handle_start_restore_frame(frame_body);
        });
    if (!started) {
        finish_restore_command_failure(QStringLiteral("service.send_failed"));
        return false;
    }
    return true;
}

RequestDisposition ServiceClient::handle_prepare_restore_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    RestorePreflightPage preflight;
    if (!parse_restore_preflight_response(root, kPrepareRestoreRequestKind, preflight)) {
        finish_restore_command_failure(parse_failure_message_code(body));
        return RequestDisposition::kFinished;
    }
    if (preflight.feasibility == kRestoreFeasibilityProvisional) {
        restore_preflight_token_ = preflight.preflight_token;
        restore_command_busy_ = false;
        restore_archive_password_.clear();
        emit restoreCommandChanged();
        const auto details = restore_preflight_details(preflight);
        const auto msg = localize_message_code(
            preflight.message_code.isEmpty() ? QStringLiteral("restore.shrink_provisional")
                                             : preflight.message_code);
        show_toast(msg);
        emit restorePreflightProvisional(details);
        return RequestDisposition::kFinished;
    }
    if (preflight.feasibility != kRestoreFeasibilityEligible || !preflight.restore_eligible ||
        preflight.preflight_token.isEmpty()) {
        finish_restore_command_failure(preflight.message_code.isEmpty()
                                           ? QStringLiteral("restore.preflight_failed")
                                           : preflight.message_code);
        return RequestDisposition::kFinished;
    }
    restore_preflight_token_ = preflight.preflight_token;
    const auto start_request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    restore_start_request_id_ = start_request_id;
    const auto start_body = encode_start_restore_request(
        start_request_id, restore_start_idempotency_key_, restore_preflight_token_,
        restore_archive_password_, restore_preserve_disk_signature_,
        restore_auto_expand_last_partition_, restore_partition_layout_edits_);
    const auto started = coordinator_->begin_request(
        start_request_id, start_body, [this](const QByteArray& frame_body) {
            return handle_start_restore_frame(frame_body);
        });
    if (!started) {
        finish_restore_command_failure(QStringLiteral("service.send_failed"));
    }
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_analyze_ntfs_shrink_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    RestorePreflightPage preflight;
    if (!parse_restore_preflight_response(root, kAnalyzeNtfsShrinkRequestKind, preflight)) {
        restore_command_busy_ = false;
        restore_archive_password_.clear();
        emit restoreCommandChanged();
        const auto msg = localize_message_code(parse_failure_message_code(body));
        show_toast(msg, true);
        emit ntfsShrinkAnalyzeFailed(msg);
        return RequestDisposition::kFinished;
    }
    restore_command_busy_ = false;
    emit restoreCommandChanged();
    if (preflight.feasibility == kRestoreFeasibilityProvisional &&
        preflight.minimum_target_bytes > preflight.target_capacity_bytes) {
        restore_preflight_token_.clear();
        restore_archive_password_.clear();
        emit ntfsShrinkAnalyzeSucceeded(restore_preflight_details(preflight));
        return RequestDisposition::kFinished;
    }
    if (preflight.feasibility != kRestoreFeasibilityEligible || !preflight.restore_eligible ||
        preflight.shrink_plan_digest.isEmpty() || preflight.preflight_token.isEmpty()) {
        restore_archive_password_.clear();
        const auto code = preflight.message_code.isEmpty()
                              ? QStringLiteral("restore.shrink_analyze_failed")
                              : preflight.message_code;
        const auto msg = localize_message_code(code);
        show_toast(msg, true);
        emit ntfsShrinkAnalyzeFailed(msg);
        return RequestDisposition::kFinished;
    }
    restore_preflight_token_ = preflight.preflight_token;
    emit ntfsShrinkAnalyzeSucceeded(restore_preflight_details(preflight));
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_start_restore_frame(const QByteArray& body) {
    if (!is_command_accepted(body)) {
        finish_restore_command_failure(parse_failure_message_code(body));
        return RequestDisposition::kFinished;
    }
    restore_command_busy_ = false;
    restore_archive_password_.clear();
    emit restoreCommandChanged();
    //% "Restore started"
    const auto msg = qtTrId("aegra.restore.started");
    show_toast(msg);
    emit restoreStartSucceeded();
    refreshJobs();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_restore_command_failure(const QString& message_code) {
    restore_command_busy_ = false;
    restore_prepare_only_ = false;
    restore_archive_password_.clear();
    emit restoreCommandChanged();
    const auto msg = localize_message_code(message_code);
    show_toast(msg, true);
    emit restoreStartFailed(msg);
}

void ServiceClient::finish_restore_preflight_failure(const QString& message_code) {
    restore_command_busy_ = false;
    restore_prepare_only_ = false;
    restore_preflight_token_.clear();
    restore_archive_password_.clear();
    emit restoreCommandChanged();
    const auto msg = localize_message_code(message_code);
    show_toast(msg, true);
    emit restorePreflightFailed(msg);
}

} // namespace aegra::desktop
