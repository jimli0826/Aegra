#include "client/pe_restore_controller.h"

#include "client/service_client.h"
#include "client/service_protocol.h"
#include "locale/message_code_map.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QUuid>

namespace aegra::desktop {
namespace {

[[nodiscard]] QString pe_failure_message_code(const QByteArray& body) {
    const auto document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return QStringLiteral("pe_restore.command_failed");
    }
    const auto code = document.object().value(QStringLiteral("message_code")).toString();
    return code.isEmpty() ? QStringLiteral("pe_restore.command_failed") : code;
}

/// Desktop locale in the "zh-CN" wire form the PE UI expects.
[[nodiscard]] QString pe_ui_locale() {
    auto name = QLocale().name();
    name.replace(QLatin1Char('_'), QLatin1Char('-'));
    return name.isEmpty() ? QStringLiteral("en-US") : name;
}

} // namespace

PeRestoreController::PeRestoreController(ServiceClient& client)
    : QObject(&client), client_(client) {}

void PeRestoreController::set_available(const bool available) {
    if (available_ == available && (available || (!armed_ && target_display_.isEmpty()))) {
        return;
    }
    available_ = available;
    if (!available) {
        armed_ = false;
        target_display_.clear();
        archive_password_.clear();
        preflight_token_.clear();
    }
    emit stateChanged();
}

bool PeRestoreController::start(const int source_disk_number, const int target_disk_number,
                                const QString& recovery_point_id,
                                const QString& archive_password,
                                const bool preserve_disk_signature,
                                const bool auto_expand_last_partition) {
    if (!client_.connected()) {
        //% "Service is not connected"
        client_.show_toast(qtTrId("aegra.error.service.disconnected"), true);
        return false;
    }
    if (!available_) {
        //% "Offline system-disk restore is not available on this Service"
        client_.show_toast(qtTrId("aegra.restore.pe_capability_missing"), true);
        return false;
    }
    if (client_.restoreCommandBusy()) {
        //% "A restore command is already in progress"
        client_.show_toast(qtTrId("aegra.restore.busy"), true);
        return false;
    }
    if (source_disk_number < 0 || target_disk_number < 0 || recovery_point_id.isEmpty()) {
        //% "Select a checkpoint and map a source disk to a target disk"
        client_.show_toast(qtTrId("aegra.restore.map_required"), true);
        return false;
    }
    const auto connection_id = client_.defaultConnectionId();
    if (connection_id.isEmpty()) {
        //% "No repository connection is available"
        client_.show_toast(qtTrId("aegra.restore.no_repository"), true);
        return false;
    }

    client_.restore_command_busy_ = true;
    preflight_token_.clear();
    archive_password_ = archive_password;
    preserve_disk_signature_ = preserve_disk_signature;
    auto_expand_last_partition_ = auto_expand_last_partition;
    arm_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit client_.restoreCommandChanged();

    const auto target_source_id = QStringLiteral("disk.%1").arg(target_disk_number);
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto body =
        encode_prepare_pe_restore_request(request_id, connection_id, recovery_point_id,
                                          target_source_id, source_disk_number, archive_password);
    const auto started = client_.coordinator_->begin_request(
        request_id, body,
        [this](const QByteArray& frame_body) { return handle_prepare_frame(frame_body); });
    if (!started) {
        finish_failure(QStringLiteral("service.send_failed"));
        return false;
    }
    return true;
}

RequestDisposition PeRestoreController::handle_prepare_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    RestorePreflightPage preflight;
    if (!parse_restore_preflight_response(root, kPreparePeRestoreRequestKind, preflight)) {
        finish_failure(pe_failure_message_code(body));
        return RequestDisposition::kFinished;
    }
    if (preflight.feasibility != kRestoreFeasibilityEligible || !preflight.restore_eligible ||
        preflight.preflight_token.isEmpty()) {
        finish_failure(preflight.message_code.isEmpty()
                           ? QStringLiteral("pe_restore.preflight_failed")
                           : preflight.message_code);
        return RequestDisposition::kFinished;
    }
    preflight_token_ = preflight.preflight_token;
    const auto arm_request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto arm_body = encode_arm_pe_restore_request(
        arm_request_id, arm_idempotency_key_, preflight_token_, archive_password_,
        /*prompt_for_password=*/false, preserve_disk_signature_, auto_expand_last_partition_,
        pe_ui_locale());
    const auto started = client_.coordinator_->begin_request(
        arm_request_id, arm_body,
        [this](const QByteArray& frame_body) { return handle_arm_frame(frame_body); });
    if (!started) {
        finish_failure(QStringLiteral("service.send_failed"));
    }
    return RequestDisposition::kFinished;
}

RequestDisposition PeRestoreController::handle_arm_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, kArmPeRestoreRequestKind)) {
        finish_failure(pe_failure_message_code(body));
        return RequestDisposition::kFinished;
    }
    CommandAck ack;
    if (!parse_command_ack_response(root, kArmPeRestoreRequestKind, ack)) {
        return RequestDisposition::kProtocolError;
    }
    client_.restore_command_busy_ = false;
    archive_password_.clear();
    emit client_.restoreCommandChanged();
    //% "Offline restore is armed. Restart the computer to begin."
    client_.show_toast(qtTrId("aegra.restore.pe_armed"));
    emit armSucceeded();
    refresh();
    return RequestDisposition::kFinished;
}

bool PeRestoreController::cancel() {
    if (!client_.connected() || !available_) {
        //% "Service is not connected"
        client_.show_toast(qtTrId("aegra.error.service.disconnected"), true);
        return false;
    }
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto body = encode_cancel_pe_restore_request(
        request_id, QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto started = client_.coordinator_->begin_request(
        request_id, body,
        [this](const QByteArray& frame_body) { return handle_cancel_frame(frame_body); });
    if (!started) {
        client_.show_toast(localize_message_code(QStringLiteral("service.send_failed")), true);
    }
    return started;
}

RequestDisposition PeRestoreController::handle_cancel_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, kCancelPeRestoreRequestKind)) {
        client_.show_toast(localize_message_code(pe_failure_message_code(body)), true);
        return RequestDisposition::kFinished;
    }
    //% "Pending offline restore was cancelled"
    client_.show_toast(qtTrId("aegra.restore.pe_cancelled"));
    refresh();
    return RequestDisposition::kFinished;
}

void PeRestoreController::refresh() {
    if (!client_.connected() || !available_) {
        return;
    }
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto body = encode_get_pe_restore_state_request(request_id);
    (void)client_.coordinator_->begin_request(
        request_id, body,
        [this](const QByteArray& frame_body) { return handle_state_frame(frame_body); });
}

RequestDisposition PeRestoreController::handle_state_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    PeRestoreStatePage state;
    if (!parse_pe_restore_state_response(root, state)) {
        // State refresh is best-effort UI polish; never toast on failure.
        return RequestDisposition::kFinished;
    }
    if (armed_ != state.armed || target_display_ != state.target_display) {
        armed_ = state.armed;
        target_display_ = state.target_display;
        emit stateChanged();
    }
    return RequestDisposition::kFinished;
}

void PeRestoreController::finish_failure(const QString& message_code) {
    client_.restore_command_busy_ = false;
    archive_password_.clear();
    preflight_token_.clear();
    emit client_.restoreCommandChanged();
    const auto message = localize_message_code(message_code);
    client_.show_toast(message, true);
    emit armFailed(message);
}

} // namespace aegra::desktop
