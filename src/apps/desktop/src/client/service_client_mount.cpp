#include "client/service_client.h"

#include "client/service_protocol.h"
#include "locale/message_code_map.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QVariantMap>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>

namespace aegra::desktop {
namespace {

// Mount Host + Dokan can exceed the default 30s coordinator deadline (old MountBackend: 120s).
constexpr int kMountCommandDeadlineMs = 120'000;

[[nodiscard]] QString parse_failure_message_code(const QByteArray& body) {
    const auto document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return QStringLiteral("service.request_failed");
    }
    const auto code = document.object().value(QStringLiteral("message_code")).toString();
    return code.isEmpty() ? QStringLiteral("service.request_failed") : code;
}

/// Wire letter is a single A–Z; UI may pass "Z" or "Z:" (old MountBackend used colon form).
[[nodiscard]] QString normalize_preferred_drive_letter(QString letter) {
    letter = letter.trimmed().toUpper();
    if (letter.size() == 2 && letter[1] == QLatin1Char(':')) {
        letter = letter.left(1);
    }
    return letter;
}

[[nodiscard]] bool parse_source_disk_number(const QVariant& value, int& out) {
    bool ok = false;
    if (value.userType() == QMetaType::Double || value.userType() == QMetaType::Float) {
        const auto as_double = value.toDouble(&ok);
        if (!ok || !std::isfinite(as_double) || as_double < 0.0 || as_double > 1.0e9) {
            return false;
        }
        out = static_cast<int>(as_double);
        return static_cast<double>(out) == as_double;
    }
    const auto as_int = value.toInt(&ok);
    if (ok && as_int >= 0) {
        out = as_int;
        return true;
    }
    const auto as_ll = value.toLongLong(&ok);
    if (ok && as_ll >= 0 && as_ll <= static_cast<qint64>((std::numeric_limits<int>::max)())) {
        out = static_cast<int>(as_ll);
        return true;
    }
    return false;
}

[[nodiscard]] QString mount_state_text(const qint64 state) {
    switch (state) {
    case kMountSessionStateMounting:
        //% "Mounting"
        return qtTrId("aegra.mount.state.mounting");
    case kMountSessionStateMounted:
        //% "Mounted"
        return qtTrId("aegra.mount.state.mounted");
    case kMountSessionStateUnmounting:
        //% "Unmounting"
        return qtTrId("aegra.mount.state.unmounting");
    case kMountSessionStateFailed:
        //% "Failed"
        return qtTrId("aegra.mount.state.failed");
    default:
        return QString::number(state);
    }
}

void enrich_mount_session(QVariantMap& row) {
    const auto state = row.value(QStringLiteral("state")).toLongLong();
    row.insert(QStringLiteral("stateText"), mount_state_text(state));
    row.insert(QStringLiteral("selected"), false);
    const auto disk = row.value(QStringLiteral("sourceDiskNumber")).toLongLong();
    if (disk >= 0) {
        //% "Disk %1"
        row.insert(QStringLiteral("diskName"),
                   qtTrId("aegra.mount.disk_name").arg(static_cast<int>(disk)));
    } else {
        row.insert(QStringLiteral("diskName"), QString{});
    }
    const auto size = row.value(QStringLiteral("diskSizeBytes")).toULongLong();
    if (size > 0) {
        const double gb = static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0);
        if (gb >= 1.0) {
            row.insert(QStringLiteral("sizeText"),
                       QString::number(gb, 'f', 1) + QStringLiteral(" GB"));
        } else {
            const double mb = static_cast<double>(size) / (1024.0 * 1024.0);
            row.insert(QStringLiteral("sizeText"),
                       QString::number(mb, 'f', 1) + QStringLiteral(" MB"));
        }
    } else {
        row.insert(QStringLiteral("sizeText"), QString{});
    }
}

} // namespace

void ServiceClient::refreshMountSessions() {
    if (state_ != State::kReady || !mount_list_available_) {
        return;
    }
    start_mount_session_query();
}

QVariantList ServiceClient::availableDriveLetters() const {
    QVariantList options;
    {
        QVariantMap auto_opt;
        //% "Auto"
        auto_opt.insert(QStringLiteral("label"), qtTrId("aegra.mount.drive_letter_auto"));
        auto_opt.insert(QStringLiteral("value"), QString{});
        options.push_back(auto_opt);
    }
#ifdef Q_OS_WIN
    // Free letters only (old MountBackend::driveLetterOptions): skip A:/B: and assigned drives.
    const DWORD mask = GetLogicalDrives();
    for (int index = 2; index < 26; ++index) {
        if ((mask & (1u << static_cast<DWORD>(index))) != 0U) {
            continue;
        }
        const auto letter = QChar(QLatin1Char(static_cast<char>('A' + index)));
        // UI value keeps "Z:" form (old MountBackend); wire path normalizes to a single letter.
        const auto with_colon = QString(letter) + QLatin1Char(':');
        QVariantMap option;
        option.insert(QStringLiteral("label"), with_colon);
        option.insert(QStringLiteral("value"), with_colon);
        options.push_back(option);
    }
#else
    for (int index = 2; index < 26; ++index) {
        const auto letter = QChar(QLatin1Char(static_cast<char>('A' + index)));
        const auto with_colon = QString(letter) + QLatin1Char(':');
        QVariantMap option;
        option.insert(QStringLiteral("label"), with_colon);
        option.insert(QStringLiteral("value"), with_colon);
        options.push_back(option);
    }
#endif
    return options;
}

void ServiceClient::clear_mount_disk_queue() {
    mount_disk_queue_.clear();
    mount_queue_recovery_point_id_.clear();
    mount_queue_archive_password_.clear();
    mount_queue_preferred_letter_.clear();
    mount_queue_preferred_applied_ = false;
    mount_queue_ok_count_ = 0;
    mount_queue_skip_count_ = 0;
    mount_queue_fail_count_ = 0;
}

bool ServiceClient::is_disk_already_mounted(const int source_disk_number,
                                            const QString& recovery_point_id) const {
    for (const auto& item : mount_sessions_) {
        const auto map = item.toMap();
        if (map.value(QStringLiteral("sourceDiskNumber")).toInt() != source_disk_number) {
            continue;
        }
        if (map.value(QStringLiteral("recoveryPointId")).toString() == recovery_point_id) {
            return true;
        }
    }
    return false;
}

void ServiceClient::finish_mount_disk_batch() {
    const auto ok_count = mount_queue_ok_count_;
    const auto skip_count = mount_queue_skip_count_;
    const auto fail_count = mount_queue_fail_count_;
    const auto last_error = mount_command_error_code_;
    clear_mount_disk_queue();
    mount_command_busy_ = false;
    mount_command_request_id_.clear();
    mount_command_idempotency_key_.clear();
    emit mountCommandChanged();
    emit loadingChanged();
    if (ok_count > 0) {
        //% "Mount started"
        show_toast(qtTrId("aegra.mount.started"));
        emit mountStartSucceeded(QString{});
        if (fail_count > 0 && !last_error.isEmpty()) {
            show_toast(localize_message_code(last_error));
        }
    } else if (fail_count > 0) {
        const auto text = last_error.isEmpty() ? localize_message_code(QStringLiteral("mount.command_failed"))
                                               : localize_message_code(last_error);
        emit mountStartFailed(text);
        show_toast(text);
    } else if (skip_count > 0) {
        //% "Selected disk(s) are already mounted"
        show_toast(qtTrId("aegra.mount.already_mounted_selection"));
    }
    start_mount_session_query();
}

bool ServiceClient::begin_next_mount_from_queue() {
    const auto connection_id = defaultConnectionId();
    if (connection_id.isEmpty()) {
        clear_mount_disk_queue();
        finish_mount_command_failure(QStringLiteral("repository.not_configured"));
        return false;
    }

    while (!mount_disk_queue_.isEmpty()) {
        const auto disk = mount_disk_queue_.takeFirst();
        if (is_disk_already_mounted(disk, mount_queue_recovery_point_id_)) {
            ++mount_queue_skip_count_;
            continue;
        }

        QString preferred;
        if (!mount_queue_preferred_applied_ && !mount_queue_preferred_letter_.isEmpty()) {
            preferred = mount_queue_preferred_letter_;
            mount_queue_preferred_applied_ = true;
        }

        // Keep last failure code for batch summary; only success path clears it.
        mount_command_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        mount_command_request_id_ = request_id;

        const auto body = encode_mount_recovery_point_request(
            request_id, mount_command_idempotency_key_, connection_id,
            mount_queue_recovery_point_id_, disk, preferred, mount_queue_archive_password_);
        const auto started = coordinator_->begin_request(
            request_id, body,
            [this](const QByteArray& frame_body) { return handle_mount_command_frame(frame_body); },
            kMountCommandDeadlineMs);
        if (!started) {
            // Keep partial successes; only fail hard when nothing was sent.
            if (mount_queue_ok_count_ > 0 || !mount_disk_queue_.isEmpty()) {
                ++mount_queue_fail_count_;
                mount_command_error_code_ = QStringLiteral("service.send_failed");
                // Drain remaining as failed so the batch can finish cleanly.
                mount_queue_fail_count_ += mount_disk_queue_.size();
                mount_disk_queue_.clear();
                finish_mount_disk_batch();
                return false;
            }
            clear_mount_disk_queue();
            finish_mount_command_failure(QStringLiteral("service.send_failed"));
            return false;
        }
        return true;
    }

    // Queue drained (all skipped / completed / failed).
    const auto had_work =
        mount_queue_ok_count_ > 0 || mount_queue_skip_count_ > 0 || mount_queue_fail_count_ > 0;
    finish_mount_disk_batch();
    return had_work;
}

bool ServiceClient::startMount(const int source_disk_number, const QString& recovery_point_id,
                               const QString& preferred_drive_letter,
                               const QString& archive_password) {
    return startMountDisks(QVariantList{source_disk_number}, recovery_point_id,
                           preferred_drive_letter, archive_password);
}

bool ServiceClient::startMountDisks(const QVariantList& source_disk_numbers,
                                    const QString& recovery_point_id,
                                    const QString& preferred_drive_letter,
                                    const QString& archive_password) {
    if (state_ != State::kReady) {
        //% "Service is not connected"
        show_toast(qtTrId("aegra.error.service.disconnected"));
        return false;
    }
    if (!mount_start_available_) {
        //% "Service does not support mount"
        show_toast(qtTrId("aegra.mount.capability_missing"));
        return false;
    }
    if (mount_command_busy_) {
        //% "A mount command is already in progress"
        show_toast(qtTrId("aegra.mount.busy"));
        return false;
    }
    if (recovery_point_id.isEmpty() || source_disk_numbers.isEmpty()) {
        //% "Select a checkpoint and at least one source disk"
        show_toast(qtTrId("aegra.mount.select_required"));
        return false;
    }
    const auto preferred = normalize_preferred_drive_letter(preferred_drive_letter);
    if (!preferred.isEmpty() &&
        (preferred.size() != 1 || preferred[0] < QLatin1Char('A') ||
         preferred[0] > QLatin1Char('Z'))) {
        //% "Preferred drive letter must be a single letter A–Z"
        show_toast(qtTrId("aegra.mount.invalid_drive_letter"));
        return false;
    }
    if (defaultConnectionId().isEmpty()) {
        //% "No repository connection is available"
        show_toast(qtTrId("aegra.restore.no_repository"));
        return false;
    }

    QSet<int> unique;
    QVector<int> disks;
    disks.reserve(source_disk_numbers.size());
    for (const auto& value : source_disk_numbers) {
        int disk = -1;
        if (!parse_source_disk_number(value, disk) || unique.contains(disk)) {
            continue;
        }
        unique.insert(disk);
        disks.push_back(disk);
    }
    std::sort(disks.begin(), disks.end());
    if (disks.isEmpty()) {
        //% "Select a checkpoint and at least one source disk"
        show_toast(qtTrId("aegra.mount.select_required"));
        return false;
    }

    clear_mount_disk_queue();
    mount_disk_queue_ = std::move(disks);
    mount_queue_recovery_point_id_ = recovery_point_id;
    mount_queue_archive_password_ = archive_password;
    mount_queue_preferred_letter_ = preferred;
    mount_queue_preferred_applied_ = false;
    mount_queue_ok_count_ = 0;
    mount_queue_skip_count_ = 0;
    mount_queue_fail_count_ = 0;

    mount_command_busy_ = true;
    mount_command_error_code_.clear();
    emit mountCommandChanged();
    emit loadingChanged();

    if (!begin_next_mount_from_queue()) {
        return false;
    }
    return true;
}

bool ServiceClient::unmountSession(const QString& session_id) {
    if (state_ != State::kReady) {
        //% "Service is not connected"
        show_toast(qtTrId("aegra.error.service.disconnected"));
        return false;
    }
    if (!mount_unmount_available_) {
        //% "Service does not support unmount"
        show_toast(qtTrId("aegra.mount.capability_missing"));
        return false;
    }
    if (mount_command_busy_) {
        //% "A mount command is already in progress"
        show_toast(qtTrId("aegra.mount.busy"));
        return false;
    }
    if (session_id.isEmpty()) {
        //% "Select a mounted session to unmount"
        show_toast(qtTrId("aegra.mount.select_session"));
        return false;
    }

    mount_command_busy_ = true;
    mount_command_error_code_.clear();
    unmount_command_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    unmount_command_request_id_ = request_id;
    emit mountCommandChanged();
    emit loadingChanged();

    const auto body =
        encode_unmount_session_request(request_id, unmount_command_idempotency_key_, session_id);
    const auto started = coordinator_->begin_request(
        request_id, body,
        [this](const QByteArray& frame_body) { return handle_unmount_command_frame(frame_body); },
        kMountCommandDeadlineMs);
    if (!started) {
        finish_mount_command_failure(QStringLiteral("service.send_failed"));
        return false;
    }
    return true;
}

void ServiceClient::start_mount_session_query() {
    if (state_ != State::kReady || !mount_list_available_ || mount_sessions_loading_) {
        return;
    }
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    mount_list_request_id_ = request_id;
    mount_sessions_loading_ = true;
    mount_sessions_error_code_.clear();
    emit mountSessionsChanged();
    const auto body = encode_mount_session_list_request(request_id);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_mount_list_frame(frame_body);
        });
    if (!started) {
        finish_mount_list_failure(QStringLiteral("service.send_failed"));
    }
}

void ServiceClient::reset_mount_sessions() {
    mount_sessions_loading_ = false;
    mount_list_request_id_.clear();
    mount_sessions_error_code_.clear();
    if (!mount_sessions_.isEmpty()) {
        mount_sessions_.clear();
    }
    emit mountSessionsChanged();
}

void ServiceClient::reset_mount_command() {
    mount_command_busy_ = false;
    mount_command_error_code_.clear();
    mount_command_request_id_.clear();
    mount_command_idempotency_key_.clear();
    unmount_command_request_id_.clear();
    unmount_command_idempotency_key_.clear();
    clear_mount_disk_queue();
    emit mountCommandChanged();
    emit loadingChanged();
}

void ServiceClient::finish_mount_command_failure(const QString& message_code) {
    mount_command_busy_ = false;
    mount_command_error_code_ = message_code;
    mount_command_request_id_.clear();
    unmount_command_request_id_.clear();
    clear_mount_disk_queue();
    emit mountCommandChanged();
    emit loadingChanged();
    const auto text = localize_message_code(message_code);
    emit mountStartFailed(text);
    show_toast(text, true);
}

void ServiceClient::finish_mount_list_failure(const QString& message_code) {
    mount_sessions_loading_ = false;
    mount_list_request_id_.clear();
    mount_sessions_error_code_ = message_code;
    emit mountSessionsChanged();
}

RequestDisposition ServiceClient::handle_mount_list_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_mount_list_failure_response(root)) {
        finish_mount_list_failure(root.value(QStringLiteral("message_code")).toString());
        return RequestDisposition::kFinished;
    }
    MountSessionPage page;
    if (!parse_mount_session_list_response(root, page)) {
        // Soft domain failure: keep the Service session so MountPage layout stays populated.
        finish_mount_list_failure(QStringLiteral("mount.list_failed"));
        return RequestDisposition::kFinished;
    }
    for (auto& item : page.items) {
        auto map = item.toMap();
        enrich_mount_session(map);
        item = map;
    }
    mount_sessions_ = std::move(page.items);
    mount_sessions_loading_ = false;
    mount_list_request_id_.clear();
    mount_sessions_error_code_.clear();
    emit mountSessionsChanged();
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_mount_command_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, kMountRecoveryPointRequestKind)) {
        // Old MountBackend continues the remaining disks after a per-disk failure.
        mount_command_error_code_ = parse_failure_message_code(body);
        ++mount_queue_fail_count_;
        mount_command_request_id_.clear();
        if (!mount_disk_queue_.isEmpty() || mount_queue_ok_count_ > 0 ||
            mount_queue_skip_count_ > 0) {
            // Defer next disk so coordinator can clear this request_id first.
            QTimer::singleShot(0, this, [this]() {
                if (!mount_command_busy_) {
                    return;
                }
                if (!begin_next_mount_from_queue() && mount_command_busy_) {
                    finish_mount_disk_batch();
                }
            });
            return RequestDisposition::kFinished;
        }
        finish_mount_command_failure(mount_command_error_code_);
        return RequestDisposition::kFinished;
    }
    CommandAck ack;
    if (!parse_command_ack_response(root, kMountRecoveryPointRequestKind, ack) ||
        !ack.has_resource_id) {
        mount_command_error_code_ = QStringLiteral("service.protocol_invalid");
        ++mount_queue_fail_count_;
        mount_command_request_id_.clear();
        if (!mount_disk_queue_.isEmpty() || mount_queue_ok_count_ > 0) {
            QTimer::singleShot(0, this, [this]() {
                if (!mount_command_busy_) {
                    return;
                }
                if (!begin_next_mount_from_queue() && mount_command_busy_) {
                    finish_mount_disk_batch();
                }
            });
            return RequestDisposition::kFinished;
        }
        finish_mount_command_failure(QStringLiteral("service.protocol_invalid"));
        return RequestDisposition::kProtocolError;
    }
    mount_command_error_code_.clear();
    mount_command_request_id_.clear();
    ++mount_queue_ok_count_;

    if (!mount_disk_queue_.isEmpty()) {
        // Keep busy=true for the multi-disk batch; start the next disk after this frame finishes.
        QTimer::singleShot(0, this, [this]() {
            if (!mount_command_busy_) {
                return;
            }
            if (!begin_next_mount_from_queue() && mount_command_busy_) {
                finish_mount_disk_batch();
            }
        });
        return RequestDisposition::kFinished;
    }

    finish_mount_disk_batch();
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_unmount_command_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, kUnmountSessionRequestKind)) {
        finish_mount_command_failure(parse_failure_message_code(body));
        emit unmountFailed(localize_message_code(mount_command_error_code_));
        return RequestDisposition::kFinished;
    }
    CommandAck ack;
    if (!parse_command_ack_response(root, kUnmountSessionRequestKind, ack)) {
        finish_mount_command_failure(QStringLiteral("service.protocol_invalid"));
        return RequestDisposition::kProtocolError;
    }
    mount_command_busy_ = false;
    mount_command_error_code_.clear();
    unmount_command_request_id_.clear();
    emit mountCommandChanged();
    emit loadingChanged();
    //% "Unmounted"
    show_toast(qtTrId("aegra.mount.unmounted"));
    emit unmountSucceeded(ack.has_resource_id ? ack.resource_id : QString{});
    start_mount_session_query();
    return RequestDisposition::kFinished;
}

} // namespace aegra::desktop
