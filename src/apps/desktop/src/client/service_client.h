#pragma once

#include "client/models/file_browse_model.h"
#include "client/models/file_recover_model.h"
#include "client/models/job_model.h"
#include "client/models/recovery_point_model.h"
#include "client/models/repository_connection_model.h"
#include "client/models/schedule_list_model.h"
#include "client/models/source_inventory_model.h"
#include "client/service_protocol.h"
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
    /// Completed jobs for Task Log (terminal scope ListJobs).
    Q_PROPERTY(aegra::desktop::JobModel* taskLog READ taskLog CONSTANT)
    Q_PROPERTY(bool taskLogLoading READ taskLogLoading NOTIFY taskLogChanged)
    Q_PROPERTY(QString taskLogErrorText READ taskLogErrorText NOTIFY taskLogChanged)
    Q_PROPERTY(bool taskLogHasMore READ taskLogHasMore NOTIFY taskLogChanged)
    Q_PROPERTY(aegra::desktop::SourceInventoryModel* sources READ sources CONSTANT)
    Q_PROPERTY(bool inventoryLoading READ inventoryLoading NOTIFY inventoryChanged)
    Q_PROPERTY(bool inventoryAvailable READ inventoryAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString inventoryErrorText READ inventoryErrorText NOTIFY inventoryChanged)
    Q_PROPERTY(bool fileBrowseAvailable READ fileBrowseAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool fileRecoverBrowseAvailable READ fileRecoverBrowseAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool fileRestoreAvailable READ fileRestoreAvailable NOTIFY stateChanged)
    Q_PROPERTY(aegra::desktop::FileBrowseModel* fileBrowseSources READ fileBrowseSources CONSTANT)
    Q_PROPERTY(aegra::desktop::FileBrowseModel* fileRestoreTargets READ fileRestoreTargets CONSTANT)
    Q_PROPERTY(aegra::desktop::FileRecoverModel* fileRecoverEntries READ fileRecoverEntries CONSTANT)
    Q_PROPERTY(QVariantList schedules READ schedules NOTIFY schedulesChanged)
    /// Stable ListView model; enable toggle uses dataChanged (no full table reset).
    Q_PROPERTY(aegra::desktop::ScheduleListModel* scheduleList READ scheduleList CONSTANT)
    Q_PROPERTY(bool schedulesLoading READ schedulesLoading NOTIFY schedulesChanged)
    Q_PROPERTY(bool scheduleCommandBusy READ scheduleCommandBusy NOTIFY scheduleCommandChanged)
    /// True when a schedule command should show the full-window loading overlay.
    /// Enable/disable toggle is busy but must not block/flash the main window.
    Q_PROPERTY(bool scheduleCommandBlocksUi READ scheduleCommandBlocksUi NOTIFY scheduleCommandChanged)
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
    Q_PROPERTY(QString repositoryCommandErrorCode READ repositoryCommandErrorCode NOTIFY
                   repositoryCommandChanged)
    Q_PROPERTY(bool backupStartAvailable READ backupStartAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool restoreStartAvailable READ restoreStartAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool restoreCommandBusy READ restoreCommandBusy NOTIFY restoreCommandChanged)
    Q_PROPERTY(bool mountStartAvailable READ mountStartAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool mountListAvailable READ mountListAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool mountCommandBusy READ mountCommandBusy NOTIFY mountCommandChanged)
    Q_PROPERTY(bool mountSessionsLoading READ mountSessionsLoading NOTIFY mountSessionsChanged)
    Q_PROPERTY(QVariantList mountSessions READ mountSessions NOTIFY mountSessionsChanged)
    Q_PROPERTY(QString mountSessionsErrorText READ mountSessionsErrorText NOTIFY mountSessionsChanged)
    Q_PROPERTY(QString mountCommandErrorText READ mountCommandErrorText NOTIFY mountCommandChanged)
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
    Q_PROPERTY(bool toastIsError READ toastIsError NOTIFY toastChanged)
    Q_PROPERTY(bool globalLoading READ globalLoading NOTIFY loadingChanged)
    /// Server chain-aware delete plan (targets only; Desktop does not recompute dependents).
    Q_PROPERTY(bool deletePlanBusy READ deletePlanBusy NOTIFY deletePlanChanged)
    Q_PROPERTY(QVariantMap deletePlan READ deletePlan NOTIFY deletePlanChanged)
    Q_PROPERTY(QString deletePlanErrorText READ deletePlanErrorText NOTIFY deletePlanChanged)
    /// Service-owned job history retention (1/3/6 months). Hard-deletes expired terminal jobs.
    Q_PROPERTY(bool serviceSettingsAvailable READ serviceSettingsAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool serviceSettingsLoading READ serviceSettingsLoading NOTIFY serviceSettingsChanged)
    Q_PROPERTY(bool serviceSettingsBusy READ serviceSettingsBusy NOTIFY serviceSettingsChanged)
    Q_PROPERTY(int jobRetentionMonths READ jobRetentionMonths NOTIFY serviceSettingsChanged)
    Q_PROPERTY(QString serviceSettingsErrorText READ serviceSettingsErrorText NOTIFY
                   serviceSettingsChanged)

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
    [[nodiscard]] JobModel* taskLog() noexcept;
    [[nodiscard]] bool taskLogLoading() const noexcept;
    [[nodiscard]] QString taskLogErrorText() const;
    [[nodiscard]] bool taskLogHasMore() const noexcept;
    /// Reload Task Log: time_index 0=all,1=24h,2=7d,3=30d; type 0=all,1=backup,2=restore,3=verify;
    /// status 0=all,1=succeeded,2=failed,3=cancelled(+interrupted grouped as cancelled filter uses cancelled only).
    Q_INVOKABLE void refreshTaskLog(int time_index, int type_index, int status_index);
    Q_INVOKABLE void loadMoreTaskLog();
    [[nodiscard]] bool serviceSettingsAvailable() const noexcept;
    [[nodiscard]] bool serviceSettingsLoading() const noexcept;
    [[nodiscard]] bool serviceSettingsBusy() const noexcept;
    [[nodiscard]] int jobRetentionMonths() const noexcept;
    [[nodiscard]] QString serviceSettingsErrorText() const;
    Q_INVOKABLE void refreshServiceSettings();
    /// Persist retention months (1, 3, or 6). Service purges expired terminal jobs immediately.
    Q_INVOKABLE bool setJobRetentionMonths(int months);
    [[nodiscard]] SourceInventoryModel* sources() noexcept;
    [[nodiscard]] bool inventoryLoading() const noexcept;
    [[nodiscard]] bool inventoryAvailable() const noexcept;
    [[nodiscard]] QString inventoryErrorText() const;
    [[nodiscard]] bool fileBrowseAvailable() const noexcept;
    [[nodiscard]] bool fileRecoverBrowseAvailable() const noexcept;
    [[nodiscard]] bool fileRestoreAvailable() const noexcept;
    [[nodiscard]] FileBrowseModel* fileBrowseSources() noexcept;
    [[nodiscard]] FileBrowseModel* fileRestoreTargets() noexcept;
    [[nodiscard]] FileRecoverModel* fileRecoverEntries() noexcept;
    [[nodiscard]] QVariantList schedules() const;
    [[nodiscard]] ScheduleListModel* scheduleList() noexcept;
    [[nodiscard]] bool schedulesLoading() const noexcept;
    [[nodiscard]] bool scheduleCommandBusy() const noexcept;
    [[nodiscard]] bool scheduleCommandBlocksUi() const noexcept;
    [[nodiscard]] bool schedulesAvailable() const noexcept;
    [[nodiscard]] QString schedulesErrorText() const;
    [[nodiscard]] RepositoryConnectionModel* connections() noexcept;
    [[nodiscard]] bool connectionsLoading() const noexcept;
    [[nodiscard]] bool connectionsAvailable() const noexcept;
    [[nodiscard]] QString connectionsErrorText() const;
    [[nodiscard]] QString selectedRepositoryConnectionId() const;
    [[nodiscard]] bool repositoryCommandBusy() const noexcept;
    [[nodiscard]] QString repositoryCommandErrorText() const;
    [[nodiscard]] QString repositoryCommandErrorCode() const;
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
    [[nodiscard]] bool restoreStartAvailable() const noexcept;
    [[nodiscard]] bool restoreCommandBusy() const noexcept;
    [[nodiscard]] bool mountStartAvailable() const noexcept;
    [[nodiscard]] bool mountListAvailable() const noexcept;
    [[nodiscard]] bool mountCommandBusy() const noexcept;
    [[nodiscard]] bool mountSessionsLoading() const noexcept;
    [[nodiscard]] QVariantList mountSessions() const;
    [[nodiscard]] QString mountSessionsErrorText() const;
    [[nodiscard]] QString mountCommandErrorText() const;
    [[nodiscard]] bool splashVisible() const noexcept;
    [[nodiscard]] bool splashBusy() const noexcept;
    [[nodiscard]] QString splashStatusText() const;
    [[nodiscard]] QString splashErrorText() const;
    [[nodiscard]] bool toastVisible() const noexcept;
    [[nodiscard]] QString toastText() const;
    [[nodiscard]] bool toastIsError() const noexcept;
    [[nodiscard]] bool globalLoading() const noexcept;
    [[nodiscard]] bool recoveryPointLayoutLoading() const noexcept;
    [[nodiscard]] QVariantList recoveryPointSourceDisks() const;
    [[nodiscard]] QVariantList recoveryPointSourceVolumes() const;
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
    Q_PROPERTY(QVariantList recoveryPointSourceVolumes READ recoveryPointSourceVolumes NOTIFY
                   recoveryPointLayoutChanged)
    Q_PROPERTY(QString recoveryPointLayoutErrorText READ recoveryPointLayoutErrorText NOTIFY
                   recoveryPointLayoutChanged)
    /// Create or update a schedule (empty scheduleId creates). Returns false if not sent.
    Q_INVOKABLE bool upsertSchedule(const QString& schedule_id, const QString& display_name,
                                     bool enabled, const QVariantList& source_ids,
                                     const QString& connection_id, const QString& frequency,
                                     const QString& time_of_day,
                                     bool exclude_page_and_hibernation_files = true,
                                     bool deduplication_enabled = true,
                                     bool encryption_enabled = false,
                                     const QString& archive_password = {},
                                     int backup_type = 2, int weekday_mask = 0,
                                     unsigned int day_of_month_mask = 0);
    /// Creates one schedule containing all selected volumes.
    /// After-create StartBackup requests Incremental; Service demotes the first run to Full.
    Q_INVOKABLE bool createSchedule(const QVariantList& sources, const QString& connection_id,
                                    const QString& frequency, const QString& time_of_day,
                                    bool exclude_page_and_hibernation_files = true,
                                    bool deduplication_enabled = true,
                                    bool encryption_enabled = false,
                                    const QString& archive_password = {},
                                    bool start_full_backup_after_create = false,
                                    int weekday_mask = 0, unsigned int day_of_month_mask = 0);
    /// Creates a file_set schedule from the current fileBrowseSources selection (opaque tokens).
    /// deduplication_enabled is always forced false for file_set.
    Q_INVOKABLE bool createFileSetSchedule(const QString& connection_id, const QString& frequency,
                                           const QString& time_of_day,
                                           bool exclude_page_and_hibernation_files = true,
                                           bool encryption_enabled = false,
                                           const QString& archive_password = {},
                                           bool start_full_backup_after_create = false,
                                           int weekday_mask = 0,
                                           unsigned int day_of_month_mask = 0);
    /// Updates a file_set schedule (source frozen; file_selections omitted).
    Q_INVOKABLE bool updateFileSetSchedule(const QString& schedule_id, const QString& display_name,
                                           bool enabled, const QString& connection_id,
                                           const QString& frequency, const QString& time_of_day,
                                           bool exclude_page_and_hibernation_files,
                                           bool encryption_enabled, int weekday_mask,
                                           unsigned int day_of_month_mask = 0);
    Q_INVOKABLE bool deleteSchedule(const QString& schedule_id);
    Q_INVOKABLE bool setScheduleEnabled(const QString& schedule_id, bool enabled);
    /// Authoritative source_ids for a listed schedule (avoids QML modelData list loss).
    Q_INVOKABLE QVariantList sourceIdsForSchedule(const QString& schedule_id) const;
    /// file_set display_chain lists from the Service schedule store (not QML modelData).
    Q_INVOKABLE QVariantList displayChainsForSchedule(const QString& schedule_id) const;
    Q_INVOKABLE void logScheduleEdit(const QString& message);
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
    /// Disk→disk restore: source Manifest disk_number → local inventory disk.N target.
    /// Tip recovery point may be Full or Incremental (Service resolves base-first chain).
    Q_INVOKABLE bool startDiskRestore(int source_disk_number, int target_disk_number,
                                      const QString& recovery_point_id,
                                      const QString& archive_password = {},
                                      bool preserve_disk_signature = true,
                                      bool auto_expand_last_partition = true);
    /// Volume→volume restore: Manifest source_volume_index → inventory vol.* target_source_id.
    Q_INVOKABLE bool startVolumeRestore(int source_volume_index, const QString& target_source_id,
                                        const QString& recovery_point_id,
                                        const QString& archive_password = {});
    /// Mount a recovery-point disk read-only via Mount Host (preferred letter optional).
    Q_INVOKABLE bool startMount(int source_disk_number, const QString& recovery_point_id,
                                const QString& preferred_drive_letter = {},
                                const QString& archive_password = {});
    /// Mount one or more source disks (old MountBackend multi-select). Preferred letter applies
    /// only to the first disk; later disks auto-assign. Busy stays true for the whole batch.
    Q_INVOKABLE bool startMountDisks(const QVariantList& source_disk_numbers,
                                     const QString& recovery_point_id,
                                     const QString& preferred_drive_letter = {},
                                     const QString& archive_password = {});
    Q_INVOKABLE bool unmountSession(const QString& session_id);
    Q_INVOKABLE void refreshMountSessions();
    /// Free drive letters for Mount Options (Auto + unused C:–Z:), matching old MountBackend.
    Q_INVOKABLE QVariantList availableDriveLetters() const;
    /// Local drive roots for Add Repository browse (Desktop Qt only; not Service IPC).
    Q_INVOKABLE QVariantList listLocalRepositoryDrives() const;
    /// Immediate subdirectories under path for Add Repository browse (Desktop Qt only).
    Q_INVOKABLE QVariantList listLocalRepositoryFolders(const QString& path) const;
    /// Locale-aware byte size for QML stat cards.
    Q_INVOKABLE QString formatBytes(qint64 bytes) const;
    /// Free space on unique volumes hosting registered repository locators (OS volume query).
    Q_INVOKABLE qint64 repositoryHostFreeBytes() const;
    /// Used space of unique volumes hosting registered repository locators
    /// (volume total − free; never recursively scans repository files).
    Q_INVOKABLE qint64 repositoryHostUsedBytes() const;
    /// Free space on the volume that contains the repository locator (OS volume query).
    Q_INVOKABLE qint64 freeBytesForLocator(const QString& locator) const;
    Q_INVOKABLE QString freeSpaceTextForLocator(const QString& locator) const;
    Q_INVOKABLE void cancelActiveBackup();
    Q_INVOKABLE void dismissToast();
    /// Show a top toast. Pass isError=true for validation/command failures (red banner).
    Q_INVOKABLE void showToast(const QString& text, bool isError = false);
    /// Prepare file restore only (capacity / eligibility). Does not start the job.
    /// Emits restorePreflightSucceeded or restorePreflightFailed when finished.
    Q_INVOKABLE bool prepareFileRestore(const QString& recovery_point_id, int conflict_policy = 1,
                                        const QString& archive_password = {},
                                        bool restore_security = true);
    /// Start file restore using a successful prepareFileRestore preflight token.
    Q_INVOKABLE bool startPreparedFileRestore();
    Q_INVOKABLE bool hasCapability(const QString& capability) const;
    Q_INVOKABLE QString defaultConnectionId() const;
    /// First selectable inventory source id, or empty.
    Q_INVOKABLE QString firstSelectableSourceId() const;
    /// Loads local file-tree roots via BrowseFileSources (backup source selection).
    Q_INVOKABLE void loadFileBrowseRoots();
    /// Loads local directory roots for file restore target (single-directory selection).
    Q_INVOKABLE void loadFileRestoreTargetRoots();
    /// Loads Recovery Point file Index roots (ListRecoveryPointEntries parent=0).
    Q_INVOKABLE void loadFileRecoverRoots(const QString& recovery_point_id,
                                          const QString& archive_password = {});
    /// Clears file-restore archive tree, target tree, and prepare token (wizard reset / Done).
    Q_INVOKABLE void clearFileRestoreState();
    /// Prepare + Start file restore from fileRecoverEntries + fileRestoreTargets selection.
    /// conflict_policy: 1 fail, 2 replace, 3 rename. restore_security defaults on.
    Q_INVOKABLE bool startFileRestore(const QString& recovery_point_id, int conflict_policy = 1,
                                      const QString& archive_password = {},
                                      bool restore_security = true);
    /// Query Service PlanDeleteRecoveryPoints for one tip RP (chain-aware targets).
    Q_INVOKABLE bool planDeleteRecoveryPoint(const QString& recovery_point_id,
                                             const QString& archive_password = {});
    /// Confirm and execute the pending delete plan token from planDeleteRecoveryPoint.
    Q_INVOKABLE bool executeDeletePlan();
    Q_INVOKABLE void clearDeletePlan();
    [[nodiscard]] bool deletePlanBusy() const noexcept;
    [[nodiscard]] QVariantMap deletePlan() const;
    [[nodiscard]] QString deletePlanErrorText() const;

  signals:
    void stateChanged();
    void repositoryChanged();
    void recoveryPointLayoutChanged();
    void jobsChanged();
    void taskLogChanged();
    void inventoryChanged();
    void schedulesChanged();
    void scheduleCommandChanged();
    /// Upsert/delete schedule accepted by Service.
    void scheduleCommandSucceeded();
    /// Upsert/delete schedule failed (localized toast already shown).
    void scheduleCommandFailed(const QString& message);
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
    void restoreCommandChanged();
    void restoreStartSucceeded();
    void restoreStartFailed(const QString& message);
    /// File restore prepare (preflight) finished successfully; token is ready for start.
    void restorePreflightSucceeded();
    /// File restore prepare failed (localized message already toasted as error).
    void restorePreflightFailed(const QString& message);
    void mountCommandChanged();
    void mountSessionsChanged();
    void mountStartSucceeded(const QString& sessionId);
    void mountStartFailed(const QString& message);
    void unmountSucceeded(const QString& sessionId);
    void unmountFailed(const QString& message);
    void deletePlanChanged();
    void deletePlanReady();
    void deleteExecuted();
    void deletePlanFailed(const QString& message);
    void serviceSettingsChanged();

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
    void start_terminal_job_seed();
    void start_task_log_query(bool append);
    void start_inventory_query();
    enum class JobQueryPurpose : std::uint8_t {
        kActive = 1,
        kTerminalSeed = 2,
        kTaskLog = 3,
    };
    [[nodiscard]] JobListQuery make_active_job_query(
        const std::optional<QString>& continuation_token) const;
    [[nodiscard]] JobListQuery make_terminal_seed_query() const;
    [[nodiscard]] JobListQuery make_task_log_query(
        const std::optional<QString>& continuation_token) const;
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
    [[nodiscard]] RequestDisposition handle_prepare_restore_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_start_restore_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_cancel_job_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_mount_list_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_mount_command_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_unmount_command_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_browse_file_sources_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_file_target_browse_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_list_recovery_point_entries_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_prepare_file_restore_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_start_file_restore_frame(const QByteArray& body);
    void on_file_browse_expand_requested(const QString& node_token);
    void on_file_target_expand_requested(const QString& node_token);
    void on_file_recover_expand_requested(const QString& entry_id);
    void reset_file_models();
    void finish_repository_failure(const QString& message_code);
    void finish_recovery_point_layout_failure(const QString& message_code);
    [[nodiscard]] RequestDisposition handle_plan_delete_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_execute_delete_plan_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_get_service_settings_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_update_service_settings_frame(const QByteArray& body);
    void finish_service_settings_failure(const QString& message_code);
    void reset_service_settings();
    void finish_plan_delete_failure(const QString& message_code);
    void finish_execute_delete_failure(const QString& message_code);
    void finish_job_failure(const QString& message_code);
    void finish_task_log_failure(const QString& message_code);
    void reset_task_log();
    void finish_inventory_failure(const QString& message_code);
    void finish_connection_failure(const QString& message_code);
    void finish_schedule_failure(const QString& message_code);
    void finish_schedule_command_failure(const QString& message_code);
    void set_schedule_command_busy(bool busy);
    void enrich_schedules_with_connections();
    /// Patch one schedule's enabled flag via ScheduleListModel::dataChanged (no list reset).
    [[nodiscard]] bool patch_schedule_enabled(const QString& schedule_id, bool enabled);
    void publish_schedules(QVariantList items);
    void enrich_job_row(JobRow& row) const;
    void finish_repository_command_failure(const QString& message_code);
    void finish_backup_command_failure(const QString& message_code);
    void finish_restore_command_failure(const QString& message_code);
    void finish_cancel_command_failure(const QString& message_code);
    void finish_mount_command_failure(const QString& message_code);
    void finish_mount_list_failure(const QString& message_code);
    void start_mount_session_query();
    void reset_mount_sessions();
    void reset_mount_command();
    void clear_mount_disk_queue();
    void finish_mount_disk_batch();
    [[nodiscard]] bool begin_next_mount_from_queue();
    [[nodiscard]] bool is_disk_already_mounted(int source_disk_number,
                                               const QString& recovery_point_id) const;
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
    void show_toast(const QString& text, bool is_error = false);
    void finish_restore_preflight_failure(const QString& message_code);
    void update_splash_for_state();

    LocaleController* locale_controller_{nullptr};
    LocaleFormat format_;
    RecoveryPointModel recovery_points_;
    JobModel jobs_;
    JobModel task_log_;
    SourceInventoryModel sources_;
    FileBrowseModel file_browse_sources_;
    FileBrowseModel file_restore_targets_;
    FileRecoverModel file_recover_entries_;
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
    QVariantList recovery_point_source_volumes_;
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
    /// When true, command success patches local enabled only (toggle), no full list reload.
    bool schedule_enable_patch_active_{false};
    QString schedule_enable_patch_id_;
    bool schedule_enable_patch_previous_{false};
    bool schedule_enable_patch_target_{false};
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
    QVariantList pending_task_log_;
    QVariantList pending_sources_;
    QVariantList pending_schedules_;
    QVariantList schedules_;
    ScheduleListModel schedule_list_;
    QVariantList pending_connections_;
    QSet<QString> toasted_job_keys_;
    std::optional<QString> requested_token_;
    std::optional<QString> job_requested_token_;
    std::optional<QString> task_log_requested_token_;
    std::optional<QString> task_log_next_token_;
    JobListQuery task_log_query_{};
    JobQueryPurpose job_query_purpose_{JobQueryPurpose::kActive};
    bool task_log_append_{false};
    std::optional<QString> inventory_requested_token_;
    std::optional<QString> schedule_requested_token_;
    std::optional<QString> connection_requested_token_;
    QString last_file_uuid_;
    QString toast_text_;
    QString active_backup_state_text_;
    QString active_backup_message_text_;
    quint32 api_version_{0};
    quint64 toast_generation_{0};
    bool toast_is_error_{false};
    /// When true, prepare handler stops after preflight (wizard Next). When false, auto-starts.
    bool restore_prepare_only_{false};
    int active_backup_progress_percent_{0};
    State state_{State::kDisconnected};
    bool repository_configured_{false};
    bool repository_loading_{false};
    bool recovery_point_layout_loading_{false};
    bool jobs_loading_{false};
    bool task_log_loading_{false};
    bool job_list_available_{false};
    bool service_settings_available_{false};
    bool service_settings_loading_{false};
    bool service_settings_busy_{false};
    int job_retention_months_{kDefaultJobRetentionMonths};
    int pending_job_retention_months_{kDefaultJobRetentionMonths};
    QString service_settings_error_code_;
    QString service_settings_request_id_;
    QString service_settings_update_request_id_;
    QString service_settings_update_idempotency_key_;
    QString task_log_error_code_;
    QString task_log_request_id_;
    bool inventory_loading_{false};
    bool inventory_available_{false};
    bool file_browse_available_{false};
    bool file_recover_browse_available_{false};
    bool file_restore_available_{false};
    QString file_browse_request_id_;
    QString file_browse_parent_token_;
    QString file_target_browse_request_id_;
    QString file_target_browse_parent_token_;
    QString file_recover_request_id_;
    QString file_recover_parent_entry_id_;
    QString file_recover_archive_password_;
    QStringList file_restore_entry_ids_;
    QString file_restore_target_token_;
    int file_restore_conflict_policy_{1};
    bool file_restore_security_{true};

    bool schedules_loading_{false};
    bool schedules_available_{false};
    bool schedule_command_busy_{false};
    bool connections_loading_{false};
    bool connections_available_{false};
    bool repository_command_busy_{false};
    int repository_command_kind_{0};
    bool delete_plan_busy_{false};
    QString delete_plan_request_id_;
    QString execute_delete_request_id_;
    QString execute_delete_idempotency_key_;
    QString delete_plan_error_code_;
    QVariantMap delete_plan_;
    bool backup_start_available_{false};
    bool restore_preflight_available_{false};
    bool restore_start_available_{false};
    bool restore_command_busy_{false};
    bool mount_list_available_{false};
    bool mount_start_available_{false};
    bool mount_unmount_available_{false};
    bool mount_command_busy_{false};
    bool mount_sessions_loading_{false};
    bool job_cancel_available_{false};
    bool backup_command_busy_{false};
    bool cancel_command_busy_{false};
    int restore_source_disk_number_{-1};
    int restore_target_disk_number_{-1};
    int restore_source_volume_index_{-1};
    bool restore_preserve_disk_signature_{true};
    bool restore_auto_expand_last_partition_{true};
    QString restore_target_source_id_;
    QString restore_recovery_point_id_;
    QString restore_archive_password_;
    QString restore_preflight_token_;
    QString restore_prepare_request_id_;
    QString restore_start_request_id_;
    QString restore_start_idempotency_key_;
    QString mount_list_request_id_;
    QString mount_command_request_id_;
    QString mount_command_idempotency_key_;
    QString unmount_command_request_id_;
    QString unmount_command_idempotency_key_;
    QString mount_command_error_code_;
    QString mount_sessions_error_code_;
    QVariantList mount_sessions_;
    /// Multi-disk mount batch (old MountBackend sequential mounts).
    QVector<int> mount_disk_queue_;
    QString mount_queue_recovery_point_id_;
    QString mount_queue_archive_password_;
    QString mount_queue_preferred_letter_;
    bool mount_queue_preferred_applied_{false};
    int mount_queue_ok_count_{0};
    int mount_queue_skip_count_{0};
    int mount_queue_fail_count_{0};
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
