#pragma once

#include "client/models/job_model.h"
#include "client/models/recovery_point_model.h"
#include "client/models/repository_connection_model.h"
#include "client/models/source_inventory_model.h"
#include "client/service_request_coordinator.h"
#include "locale/locale_format.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <optional>

class QTimer;

namespace aegra::desktop {

class IpcFrameTransport;
class LocaleController;

// Desktop composition facade over transport, protocol codec, request coordinator, and domain
// models. Supports concurrent Repository/Job/Inventory/Connection queries and backup commands.
class ServiceClient final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString serviceVersion READ serviceVersion NOTIFY stateChanged)
    Q_PROPERTY(quint32 apiVersion READ apiVersion NOTIFY stateChanged)
    Q_PROPERTY(QStringList capabilities READ capabilities NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(bool repositoryConfigured READ repositoryConfigured NOTIFY repositoryChanged)
    Q_PROPERTY(bool repositoryLoading READ repositoryLoading NOTIFY repositoryChanged)
    Q_PROPERTY(QString repositoryUuid READ repositoryUuid NOTIFY repositoryChanged)
    Q_PROPERTY(QString repositoryStatusText READ repositoryStatusText NOTIFY repositoryChanged)
    Q_PROPERTY(QString repositoryErrorText READ repositoryErrorText NOTIFY repositoryChanged)
    Q_PROPERTY(aegra::desktop::RecoveryPointModel* recoveryPoints READ recoveryPoints CONSTANT)
    Q_PROPERTY(int recoveryPointCount READ recoveryPointCount NOTIFY repositoryChanged)
    Q_PROPERTY(aegra::desktop::JobModel* jobs READ jobs CONSTANT)
    Q_PROPERTY(bool jobsLoading READ jobsLoading NOTIFY jobsChanged)
    Q_PROPERTY(bool jobListAvailable READ jobListAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString jobsErrorText READ jobsErrorText NOTIFY jobsChanged)
    Q_PROPERTY(aegra::desktop::SourceInventoryModel* sources READ sources CONSTANT)
    Q_PROPERTY(bool inventoryLoading READ inventoryLoading NOTIFY inventoryChanged)
    Q_PROPERTY(bool inventoryAvailable READ inventoryAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString inventoryErrorText READ inventoryErrorText NOTIFY inventoryChanged)
    Q_PROPERTY(QVariantList schedules READ schedules NOTIFY schedulesChanged)
    Q_PROPERTY(bool schedulesLoading READ schedulesLoading NOTIFY schedulesChanged)
    Q_PROPERTY(bool schedulesAvailable READ schedulesAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString schedulesErrorText READ schedulesErrorText NOTIFY schedulesChanged)
    Q_PROPERTY(aegra::desktop::RepositoryConnectionModel* connections READ connections CONSTANT)
    Q_PROPERTY(bool connectionsLoading READ connectionsLoading NOTIFY connectionsChanged)
    Q_PROPERTY(bool connectionsAvailable READ connectionsAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString connectionsErrorText READ connectionsErrorText NOTIFY connectionsChanged)
    Q_PROPERTY(QString selectedRepositoryConnectionId READ selectedRepositoryConnectionId NOTIFY
                   repositoryChanged)
    Q_PROPERTY(
        bool repositoryCommandBusy READ repositoryCommandBusy NOTIFY repositoryCommandChanged)
    Q_PROPERTY(QString repositoryCommandErrorText READ repositoryCommandErrorText NOTIFY
                   repositoryCommandChanged)
    Q_PROPERTY(bool backupStartAvailable READ backupStartAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool jobCancelAvailable READ jobCancelAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool backupCommandBusy READ backupCommandBusy NOTIFY backupCommandChanged)
    Q_PROPERTY(bool cancelCommandBusy READ cancelCommandBusy NOTIFY backupCommandChanged)
    Q_PROPERTY(
        QString backupCommandErrorText READ backupCommandErrorText NOTIFY backupCommandChanged)
    Q_PROPERTY(QString activeBackupJobId READ activeBackupJobId NOTIFY backupCommandChanged)
    Q_PROPERTY(QString activeBackupStateText READ activeBackupStateText NOTIFY backupObserveChanged)
    Q_PROPERTY(int activeBackupProgressPercent READ activeBackupProgressPercent NOTIFY
                   backupObserveChanged)
    Q_PROPERTY(bool activeBackupProgressVisible READ activeBackupProgressVisible NOTIFY
                   backupObserveChanged)
    Q_PROPERTY(
        QString activeBackupMessageText READ activeBackupMessageText NOTIFY backupObserveChanged)
    Q_PROPERTY(bool activeBackupTerminal READ activeBackupTerminal NOTIFY backupObserveChanged)
    Q_PROPERTY(
        bool activeBackupCancellable READ activeBackupCancellable NOTIFY backupObserveChanged)
    Q_PROPERTY(bool splashVisible READ splashVisible NOTIFY splashChanged)
    Q_PROPERTY(bool splashBusy READ splashBusy NOTIFY splashChanged)
    Q_PROPERTY(QString splashStatusText READ splashStatusText NOTIFY splashChanged)
    Q_PROPERTY(QString splashErrorText READ splashErrorText NOTIFY splashChanged)
    Q_PROPERTY(bool toastVisible READ toastVisible NOTIFY toastChanged)
    Q_PROPERTY(QString toastText READ toastText NOTIFY toastChanged)
    Q_PROPERTY(bool globalLoading READ globalLoading NOTIFY loadingChanged)

  public:
    explicit ServiceClient(QObject* parent = nullptr);
    ~ServiceClient() override;

    void set_locale_controller(LocaleController* locale_controller);

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString serviceVersion() const;
    [[nodiscard]] quint32 apiVersion() const noexcept;
    [[nodiscard]] QStringList capabilities() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] bool repositoryConfigured() const noexcept;
    [[nodiscard]] bool repositoryLoading() const noexcept;
    [[nodiscard]] QString repositoryUuid() const;
    [[nodiscard]] QString repositoryStatusText() const;
    [[nodiscard]] QString repositoryErrorText() const;
    [[nodiscard]] RecoveryPointModel* recoveryPoints() noexcept;
    [[nodiscard]] int recoveryPointCount() const;
    [[nodiscard]] JobModel* jobs() noexcept;
    [[nodiscard]] bool jobsLoading() const noexcept;
    [[nodiscard]] bool jobListAvailable() const noexcept;
    [[nodiscard]] QString jobsErrorText() const;
    [[nodiscard]] SourceInventoryModel* sources() noexcept;
    [[nodiscard]] bool inventoryLoading() const noexcept;
    [[nodiscard]] bool inventoryAvailable() const noexcept;
    [[nodiscard]] QString inventoryErrorText() const;
    [[nodiscard]] QVariantList schedules() const;
    [[nodiscard]] bool schedulesLoading() const noexcept;
    [[nodiscard]] bool schedulesAvailable() const noexcept;
    [[nodiscard]] QString schedulesErrorText() const;
    [[nodiscard]] RepositoryConnectionModel* connections() noexcept;
    [[nodiscard]] bool connectionsLoading() const noexcept;
    [[nodiscard]] bool connectionsAvailable() const noexcept;
    [[nodiscard]] QString connectionsErrorText() const;
    [[nodiscard]] QString selectedRepositoryConnectionId() const;
    [[nodiscard]] bool repositoryCommandBusy() const noexcept;
    [[nodiscard]] QString repositoryCommandErrorText() const;
    [[nodiscard]] bool backupStartAvailable() const noexcept;
    [[nodiscard]] bool jobCancelAvailable() const noexcept;
    [[nodiscard]] bool backupCommandBusy() const noexcept;
    [[nodiscard]] bool cancelCommandBusy() const noexcept;
    [[nodiscard]] QString backupCommandErrorText() const;
    [[nodiscard]] QString activeBackupJobId() const;
    [[nodiscard]] QString activeBackupStateText() const;
    [[nodiscard]] int activeBackupProgressPercent() const noexcept;
    [[nodiscard]] bool activeBackupProgressVisible() const noexcept;
    [[nodiscard]] QString activeBackupMessageText() const;
    [[nodiscard]] bool activeBackupTerminal() const noexcept;
    [[nodiscard]] bool activeBackupCancellable() const noexcept;
    [[nodiscard]] bool splashVisible() const noexcept;
    [[nodiscard]] bool splashBusy() const noexcept;
    [[nodiscard]] QString splashStatusText() const;
    [[nodiscard]] QString splashErrorText() const;
    [[nodiscard]] bool toastVisible() const noexcept;
    [[nodiscard]] QString toastText() const;
    [[nodiscard]] bool globalLoading() const noexcept;
    [[nodiscard]] bool recoveryPointLayoutLoading() const noexcept;
    [[nodiscard]] QVariantList recoveryPointSourceDisks() const;
    [[nodiscard]] QString recoveryPointLayoutErrorText() const;

    Q_INVOKABLE void reconnect();
    Q_INVOKABLE void refreshRepository();
    Q_INVOKABLE void refreshJobs();
    Q_INVOKABLE void refreshInventory();
    Q_INVOKABLE void refreshConnections();
    Q_INVOKABLE void refreshSchedules();
    /// Loads Manifest volumes for a recovery point (Restore Source Disks). Async; emits
    /// recoveryPointLayoutChanged when finished. Pass empty recovery_point_id to clear.
    Q_INVOKABLE void loadRecoveryPointLayout(const QString& recovery_point_id,
                                             const QString& archive_password = {});
    Q_PROPERTY(bool recoveryPointLayoutLoading READ recoveryPointLayoutLoading NOTIFY
                   recoveryPointLayoutChanged)
    Q_PROPERTY(QVariantList recoveryPointSourceDisks READ recoveryPointSourceDisks NOTIFY
                   recoveryPointLayoutChanged)
    Q_PROPERTY(QString recoveryPointLayoutErrorText READ recoveryPointLayoutErrorText NOTIFY
                   recoveryPointLayoutChanged)
    /// Create or update a schedule (empty scheduleId creates). Returns false if not sent.
    Q_INVOKABLE bool upsertSchedule(const QString& schedule_id, const QString& display_name,
                                     bool enabled, const QVariantList& source_ids,
                                     const QString& connection_id, const QString& frequency,
                                     const QString& time_of_day,
                                     bool exclude_page_and_hibernation_files = true,
                                     bool encryption_enabled = false,
                                     const QString& archive_password = {});
    /// Creates one schedule containing all selected volumes.
    /// When start_full_backup_after_create is true, a Full StartBackup runs after create ack.
    Q_INVOKABLE bool createSchedule(const QVariantList& sources, const QString& connection_id,
                                    const QString& frequency, const QString& time_of_day,
                                    bool exclude_page_and_hibernation_files = true,
                                    bool encryption_enabled = false,
                                    const QString& archive_password = {},
                                    bool start_full_backup_after_create = false);
    Q_INVOKABLE bool deleteSchedule(const QString& schedule_id);
    Q_INVOKABLE bool setScheduleEnabled(const QString& schedule_id, bool enabled);
    Q_INVOKABLE void selectRepositoryConnection(const QString& connection_id);
    Q_INVOKABLE void addRepositoryConnection(const QString& display_name, const QString& locator);
    Q_INVOKABLE void importRepositoryConnection(const QString& display_name,
                                                const QString& locator);
    Q_INVOKABLE void testRepositoryConnection(const QString& connection_id);
    Q_INVOKABLE void setDefaultRepositoryConnection(const QString& connection_id);
    Q_INVOKABLE void removeRepositoryConnection(const QString& connection_id);
    /// Starts a backup for an existing schedule. Wire payload is only schedule_id + backup_type;
    /// Service loads sources/repo/options/password from the schedule. backup_type: 1 full, 2 inc.
    Q_INVOKABLE bool startBackup(const QString& schedule_id, int backup_type = 1);
    Q_INVOKABLE void cancelActiveBackup();
    Q_INVOKABLE void dismissToast();
    /// Show a top toast (success/info). Safe for QML schedule Run feedback.
    Q_INVOKABLE void showToast(const QString& text);
    Q_INVOKABLE bool hasCapability(const QString& capability) const;
    Q_INVOKABLE QString defaultConnectionId() const;
    /// First selectable inventory source id, or empty.
    Q_INVOKABLE QString firstSelectableSourceId() const;

  signals:
    void stateChanged();
    void repositoryChanged();
    void recoveryPointLayoutChanged();
    void jobsChanged();
    void inventoryChanged();
    void schedulesChanged();
    void connectionsChanged();
    void repositoryCommandChanged();
    void backupCommandChanged();
    void backupObserveChanged();
    void splashChanged();
    void toastChanged();
    void loadingChanged();
    /// Service accepted backup.start and returned a job id.
    void backupStartSucceeded(const QString& jobId);
    /// backup.start rejected or failed (localized message for toast).
    void backupStartFailed(const QString& message);

  private:
    enum class State : std::uint8_t {
        kDisconnected,
        kConnecting,
        kReady,
    };

    void on_transport_connected();
    void on_transport_disconnected();
    void on_transport_error(const QString& message_code);
    void on_request_failed(const QString& message_code);
    void on_locale_changed();
    void on_job_poll_tick();
    void send_service_info_request();
    void start_repository_query();
    void start_job_query();
    void start_inventory_query();
    void start_connection_query();
    void start_schedule_query();
    [[nodiscard]] RequestDisposition handle_service_info_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_recovery_point_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_recovery_point_layout_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_job_list_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_inventory_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_connection_list_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_schedule_list_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_repository_command_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_schedule_command_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_start_backup_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_cancel_job_frame(const QByteArray& body);
    void finish_repository_failure(const QString& message_code);
    void finish_recovery_point_layout_failure(const QString& message_code);
    void finish_job_failure(const QString& message_code);
    void finish_inventory_failure(const QString& message_code);
    void finish_connection_failure(const QString& message_code);
    void finish_schedule_failure(const QString& message_code);
    void finish_schedule_command_failure(const QString& message_code);
    void enrich_schedules_with_connections();
    void enrich_job_row(JobRow& row) const;
    void finish_repository_command_failure(const QString& message_code);
    void finish_backup_command_failure(const QString& message_code);
    void finish_cancel_command_failure(const QString& message_code);
    void reset_repository();
    void reset_recovery_point_layout();
    void reset_jobs();
    void reset_inventory();
    void reset_connections();
    void reset_schedules();
    void reset_repository_command();
    void start_repository_input_command(int request_kind, const QString& display_name,
                                        const QString& locator);
    void start_repository_resource_command(int request_kind, const QString& connection_id);
    void reset_backup_command();
    void set_state(State state, QString error_code = {});
    void update_format_locale();
    void update_job_polling();
    void update_active_backup_observe();
    void publish_terminal_toasts(const QVector<JobRow>& rows);
    void seed_terminal_toast_baseline(const QVector<JobRow>& rows);
    void show_toast(const QString& text);
    void update_splash_for_state();

    LocaleController* locale_controller_{nullptr};
    LocaleFormat format_;
    RecoveryPointModel recovery_points_;
    JobModel jobs_;
    SourceInventoryModel sources_;
    RepositoryConnectionModel connections_;
    std::unique_ptr<IpcFrameTransport> transport_;
    std::unique_ptr<ServiceRequestCoordinator> coordinator_;
    QTimer* job_poll_timer_{nullptr};
    QTimer* toast_timer_{nullptr};
    QString service_version_;
    QStringList capabilities_;
    QString error_code_;
    QString repository_uuid_;
    QString repository_error_code_;
    QString recovery_point_layout_error_code_;
    QVariantList recovery_point_source_disks_;
    QString jobs_error_code_;
    QString inventory_error_code_;
    QString schedules_error_code_;
    QString connections_error_code_;
    QString repository_command_error_code_;
    QString selected_repository_connection_id_;
    QString backup_command_error_code_;
    QString repository_request_id_;
    QString recovery_point_layout_request_id_;
    QString recovery_point_layout_recovery_point_id_;
    QString job_request_id_;
    QString inventory_request_id_;
    QString schedule_request_id_;
    QString connection_request_id_;
    QString repository_command_request_id_;
    QString repository_command_idempotency_key_;
    QString schedule_command_request_id_;
    QString schedule_command_idempotency_key_;
    int schedule_command_kind_{0};
    bool start_full_backup_after_schedule_create_{false};
    QString start_backup_request_id_;
    QString cancel_job_request_id_;
    QString start_backup_idempotency_key_;
    QString cancel_job_idempotency_key_;
    QString active_backup_job_id_;
    QString pending_backup_schedule_id_;
    QStringList pending_backup_source_ids_;
    QString pending_backup_connection_id_;
    QVariantList pending_recovery_points_;
    QVariantList pending_jobs_;
    QVariantList pending_sources_;
    QVariantList pending_schedules_;
    QVariantList schedules_;
    QVariantList pending_connections_;
    QSet<QString> toasted_job_keys_;
    std::optional<QString> requested_token_;
    std::optional<QString> job_requested_token_;
    std::optional<QString> inventory_requested_token_;
    std::optional<QString> schedule_requested_token_;
    std::optional<QString> connection_requested_token_;
    QString last_file_uuid_;
    QString toast_text_;
    QString active_backup_state_text_;
    QString active_backup_message_text_;
    quint32 api_version_{0};
    quint64 toast_generation_{0};
    int active_backup_progress_percent_{0};
    State state_{State::kDisconnected};
    bool repository_configured_{false};
    bool repository_loading_{false};
    bool recovery_point_layout_loading_{false};
    bool jobs_loading_{false};
    bool job_list_available_{false};
    bool inventory_loading_{false};
    bool inventory_available_{false};
    bool schedules_loading_{false};
    bool schedules_available_{false};
    bool schedule_command_busy_{false};
    bool connections_loading_{false};
    bool connections_available_{false};
    bool repository_command_busy_{false};
    int repository_command_kind_{0};
    bool backup_start_available_{false};
    bool job_cancel_available_{false};
    bool backup_command_busy_{false};
    bool cancel_command_busy_{false};
    bool active_backup_progress_visible_{false};
    bool active_backup_terminal_{false};
    bool active_backup_cancellable_{false};
    bool handshake_complete_{false};
    bool first_ready_seen_{false};
    bool splash_error_{false};
    bool toast_visible_{false};
    bool jobs_baseline_seeded_{false};
    QTimer* reconnect_watchdog_{nullptr};
};

} // namespace aegra::desktop
