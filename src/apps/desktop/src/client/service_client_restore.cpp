#include "client/service_client.h"

#include "client/service_protocol.h"
#include "locale/message_code_map.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace aegra::desktop {
namespace {

[[nodiscard]] QString parse_preflight_token(const QByteArray& body) {
    const auto document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return {};
    }
    const auto root = document.object();
    if (root.value(QStringLiteral("kind")).toInt() != 1) {
        return {};
    }
    const auto payload = root.value(QStringLiteral("payload")).toObject();
    return payload.value(QStringLiteral("preflight_token")).toString();
}

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

} // namespace

bool ServiceClient::startDiskRestore(const int source_disk_number, const int target_disk_number,
                                     const QString& recovery_point_id,
                                     const QString& archive_password,
                                     const bool preserve_disk_signature,
                                     const bool auto_expand_last_partition) {
    if (state_ != State::kReady) {
        //% "Service is not connected"
        show_toast(qtTrId("aegra.error.service.disconnected"));
        return false;
    }
    if (!restore_start_available_ || !restore_preflight_available_) {
        //% "Service does not support restore"
        show_toast(qtTrId("aegra.restore.capability_missing"));
        return false;
    }
    if (restore_command_busy_) {
        //% "A restore command is already in progress"
        show_toast(qtTrId("aegra.restore.busy"));
        return false;
    }
    if (source_disk_number < 0 || target_disk_number < 0 || recovery_point_id.isEmpty()) {
        //% "Select a checkpoint and map a source disk to a target disk"
        show_toast(qtTrId("aegra.restore.map_required"));
        return false;
    }
    const auto connection_id = defaultConnectionId();
    if (connection_id.isEmpty()) {
        //% "No repository connection is available"
        show_toast(qtTrId("aegra.restore.no_repository"));
        return false;
    }

    restore_command_busy_ = true;
    restore_source_disk_number_ = source_disk_number;
    restore_target_disk_number_ = target_disk_number;
    restore_preserve_disk_signature_ = preserve_disk_signature;
    restore_auto_expand_last_partition_ = auto_expand_last_partition;
    restore_recovery_point_id_ = recovery_point_id;
    restore_archive_password_ = archive_password;
    restore_preflight_token_.clear();
    restore_start_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit restoreCommandChanged();

    const auto target_source_id =
        QStringLiteral("disk.%1").arg(target_disk_number);
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    restore_prepare_request_id_ = request_id;
    const auto body =
        encode_prepare_restore_request(request_id, connection_id, recovery_point_id,
                                       target_source_id, source_disk_number, archive_password);
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

RequestDisposition ServiceClient::handle_prepare_restore_frame(const QByteArray& body) {
    const auto token = parse_preflight_token(body);
    if (token.isEmpty()) {
        finish_restore_command_failure(parse_failure_message_code(body));
        return RequestDisposition::kFinished;
    }
    restore_preflight_token_ = token;
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    restore_start_request_id_ = request_id;
    const auto start_body = encode_start_restore_request(
        request_id, restore_start_idempotency_key_, restore_preflight_token_,
        restore_archive_password_, restore_preserve_disk_signature_,
        restore_auto_expand_last_partition_);
    const auto started =
        coordinator_->begin_request(request_id, start_body, [this](const QByteArray& frame_body) {
            return handle_start_restore_frame(frame_body);
        });
    if (!started) {
        finish_restore_command_failure(QStringLiteral("service.send_failed"));
    }
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
    restore_archive_password_.clear();
    emit restoreCommandChanged();
    const auto msg = localize_message_code(message_code);
    show_toast(msg);
    emit restoreStartFailed(msg);
}

} // namespace aegra::desktop
