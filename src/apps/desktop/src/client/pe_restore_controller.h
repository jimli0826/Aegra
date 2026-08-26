#pragma once

#include "client/service_request_coordinator.h"

#include <QObject>
#include <QString>

namespace aegra::desktop {

class ServiceClient;

/// WinPE offline system-disk restore client surface (kinds 19/20/51/52), exposed to
/// QML as `serviceClient.peRestore`. `start` runs PreparePeRestore then chains
/// ArmPeRestore; on success the one-time boot is armed and the user restarts the
/// machine manually. QML shows the irreversible confirmation BEFORE calling start.
class PeRestoreController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool armed READ armed NOTIFY stateChanged)
    Q_PROPERTY(bool preparing READ preparing NOTIFY preparingChanged)
    Q_PROPERTY(QString targetDisplay READ targetDisplay NOTIFY stateChanged)

  public:
    explicit PeRestoreController(ServiceClient& client);

    [[nodiscard]] bool available() const noexcept { return available_; }
    [[nodiscard]] bool armed() const noexcept { return armed_; }
    [[nodiscard]] bool preparing() const noexcept { return preparing_; }
    [[nodiscard]] QString targetDisplay() const { return target_display_; }

    Q_INVOKABLE bool start(int source_disk_number, int target_disk_number,
                           const QString& recovery_point_id, const QString& archive_password,
                           bool preserve_disk_signature, bool auto_expand_last_partition);
    /// Disarm the pending hand-off (kind 52). `silent` skips the cancelled toast
    /// (used when replacing a pending job before a new Arm).
    Q_INVOKABLE bool cancel(bool silent = false);
    /// Refresh the armed state (kind 20); best-effort, never toasts.
    Q_INVOKABLE void refresh();
    /// User-initiated reboot into the armed WinPE (SE_SHUTDOWN).
    Q_INVOKABLE bool restart_now();

    /// ServiceClient handshake / disconnect hooks.
    void set_available(bool available);

  signals:
    void stateChanged();
    void preparingChanged();
    void armSucceeded();
    void armFailed(const QString& message);
    void cancelSucceeded();
    void cancelFailed(const QString& message);

  private:
    [[nodiscard]] RequestDisposition handle_prepare_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_arm_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_state_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_cancel_frame(const QByteArray& body);
    void finish_failure(const QString& message);
    void set_preparing(bool preparing);

    ServiceClient& client_;
    bool available_{false};
    bool armed_{false};
    bool preparing_{false};
    QString target_display_;
    QString preflight_token_;
    QString archive_password_;
    bool preserve_disk_signature_{true};
    bool auto_expand_last_partition_{false};
    QString arm_idempotency_key_;
    bool silent_cancel_{false};
};

} // namespace aegra::desktop
