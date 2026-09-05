#include "client/service_client.h"

#include "client/ipc_frame_transport.h"
#include "client/pe_restore_controller.h"
#include "client/service_protocol.h"
#include "client/service_request_coordinator.h"
#include "locale/locale_controller.h"
#include "locale/message_code_map.h"

#include <QDateTime>
#include <QJsonObject>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace aegra::desktop {
namespace {

constexpr auto kServicePipeName = "aegra-service-control";
constexpr qsizetype kMaximumJobs = 10'000;
// Active-job poll: keep short so file restore/backup percent can move between quantums.
// Idle polling is off (timer only runs while has_active_jobs()).
constexpr int kJobPollIntervalMilliseconds = 500;
// Post-backup verify/boot check are submitted by the Service up to tens of
// seconds after the previous job ends; keep sampling within this window.
constexpr qint64 kPostBackupWatchMilliseconds = 120'000;
constexpr qint64 kGraceJobQueryIntervalMilliseconds = 2'000;

[[nodiscard]] QString job_toast_key(const JobRow& row) {
    return row.job_id + QLatin1Char('|') + QString::number(row.state);
}

} // namespace

ServiceClient::ServiceClient(QObject* parent)
    : QObject(parent), recovery_points_(this), jobs_(this), task_log_(this), sources_(this),
      file_browse_sources_(this), file_restore_targets_(this), file_recover_entries_(this),
      connections_(this), schedule_list_(this),
      transport_(std::make_unique<IpcFrameTransport>(QLatin1String(kServicePipeName))),
      coordinator_(std::make_unique<ServiceRequestCoordinator>(*transport_)),
      pe_restore_(std::make_unique<PeRestoreController>(*this)),
      job_poll_timer_(new QTimer(this)), toast_timer_(new QTimer(this)),
      splash_connect_timer_(new QTimer(this)),
      reconnect_watchdog_(new QTimer(this)) {
    splash_connect_timer_->setSingleShot(true);
    connect(splash_connect_timer_, &QTimer::timeout, this,
            &ServiceClient::on_splash_connect_timeout);
    recovery_points_.set_locale_format(&format_);
    jobs_.set_locale_format(&format_);
    task_log_.set_locale_format(&format_);
    sources_.set_locale_format(&format_);
    file_restore_targets_.setSingleDirectoryMode(true);
    connect(&file_browse_sources_, &FileBrowseModel::expandRequested, this,
            &ServiceClient::on_file_browse_expand_requested);
    connect(&file_restore_targets_, &FileBrowseModel::expandRequested, this,
            &ServiceClient::on_file_target_expand_requested);
    connect(&file_recover_entries_, &FileRecoverModel::expandRequested, this,
            &ServiceClient::on_file_recover_expand_requested);
    job_poll_timer_->setInterval(kJobPollIntervalMilliseconds);
    toast_timer_->setSingleShot(true);
    toast_timer_->setInterval(4'000);
    // Re-reads the hypervisor status while a Service-side probe pass is running.
    hypervisor_poll_timer_ = new QTimer(this);
    hypervisor_poll_timer_->setSingleShot(true);
    hypervisor_poll_timer_->setInterval(2'000);
    connect(hypervisor_poll_timer_, &QTimer::timeout, this,
            &ServiceClient::start_hypervisor_status_query);
    // After main UI has been Ready once, keep trying the live Service pipe if we drop offline.
    reconnect_watchdog_->setInterval(2'500);
    connect(reconnect_watchdog_, &QTimer::timeout, this, [this]() {
        if (!first_ready_seen_) {
            return;
        }
        if (state_ == State::kReady || state_ == State::kConnecting) {
            return;
        }
        reconnect();
    });
    reconnect_watchdog_->start();
    connect(job_poll_timer_, &QTimer::timeout, this, &ServiceClient::on_job_poll_tick);
    connect(toast_timer_, &QTimer::timeout, this, [this]() {
        if (toast_visible_) {
            dismissToast();
        }
    });
    connect(transport_.get(), &IpcFrameTransport::connected, this,
            &ServiceClient::on_transport_connected);
    connect(transport_.get(), &IpcFrameTransport::disconnected, this,
            &ServiceClient::on_transport_disconnected);
    connect(transport_.get(), &IpcFrameTransport::transport_error, this,
            &ServiceClient::on_transport_error);
    connect(coordinator_.get(), &ServiceRequestCoordinator::request_failed, this,
            &ServiceClient::on_request_failed);
    QTimer::singleShot(0, this, &ServiceClient::reconnect);
}

ServiceClient::~ServiceClient() = default;

void ServiceClient::set_locale_controller(LocaleController* locale_controller) {
    if (locale_controller_ != nullptr) {
        disconnect(locale_controller_, &LocaleController::languageChanged, this,
                   &ServiceClient::on_locale_changed);
    }
    locale_controller_ = locale_controller;
    if (locale_controller_ != nullptr) {
        connect(locale_controller_, &LocaleController::languageChanged, this,
                &ServiceClient::on_locale_changed);
        update_format_locale();
    }
}

bool ServiceClient::connected() const noexcept { return state_ == State::kReady; }

QString ServiceClient::statusText() const {
    switch (state_) {
    case State::kDisconnected:
        //% "Disconnected"
        return qtTrId("aegra.service.state.disconnected");
    case State::kConnecting:
        //% "Connecting"
        return qtTrId("aegra.service.state.connecting");
    case State::kReady:
        //% "Running"
        return qtTrId("aegra.service.state.running");
    }
    //% "Unknown"
    return qtTrId("aegra.common.unknown");
}

QString ServiceClient::serviceVersion() const { return service_version_; }
quint32 ServiceClient::apiVersion() const noexcept { return api_version_; }
QStringList ServiceClient::capabilities() const { return capabilities_; }
bool ServiceClient::virtualBoxInstalled() const noexcept {
    return capabilities_.contains(QStringLiteral("boot_check.hypervisor.virtualbox.installed"));
}
bool ServiceClient::hyperVInstalled() const noexcept {
    return capabilities_.contains(QStringLiteral("boot_check.hypervisor.hyperv.installed"));
}

const BootCheckHypervisorStatus* ServiceClient::hypervisor_status_for(const int hypervisor) const {
    for (const auto& status : hypervisor_status_) {
        if (status.hypervisor == hypervisor) {
            return &status;
        }
    }
    return nullptr;
}

QString ServiceClient::hypervisor_unavailable_text(const int hypervisor) const {
    const auto* status = hypervisor_status_for(hypervisor);
    if (status == nullptr || !status->installed ||
        status->probe_state != kBootCheckProbeStateProbed || status->available) {
        return {};
    }
    return localize_message_code(status->message_code.isEmpty()
                                     ? QStringLiteral("bootcheck.provider_unavailable")
                                     : status->message_code);
}

int ServiceClient::virtualBoxProbeState() const noexcept {
    const auto* status = hypervisor_status_for(1);
    return status != nullptr ? status->probe_state : kBootCheckProbeStateNotProbed;
}

QString ServiceClient::virtualBoxUnavailableText() const { return hypervisor_unavailable_text(1); }

int ServiceClient::hyperVProbeState() const noexcept {
    const auto* status = hypervisor_status_for(2);
    return status != nullptr ? status->probe_state : kBootCheckProbeStateNotProbed;
}

QString ServiceClient::hyperVUnavailableText() const { return hypervisor_unavailable_text(2); }

bool ServiceClient::hypervisorProbing() const noexcept {
    if (hypervisor_refresh_busy_) {
        return true;
    }
    return std::ranges::any_of(hypervisor_status_, [](const BootCheckHypervisorStatus& status) {
        return status.probe_state == kBootCheckProbeStateProbing;
    });
}

void ServiceClient::refreshHypervisorStatus() {
    if (state_ != State::kReady || hypervisor_refresh_busy_) {
        return;
    }
    hypervisor_refresh_busy_ = true;
    emit hypervisorStatusChanged();
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    hypervisor_refresh_request_id_ = request_id;
    const auto body = encode_refresh_boot_check_hypervisor_status_request(
        request_id, QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_hypervisor_refresh_frame(frame_body);
        })) {
        hypervisor_refresh_busy_ = false;
        emit hypervisorStatusChanged();
    }
}

void ServiceClient::start_hypervisor_status_query() {
    if (state_ != State::kReady || hypervisor_status_loading_) {
        return;
    }
    hypervisor_status_loading_ = true;
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    hypervisor_status_request_id_ = request_id;
    const auto body = encode_get_boot_check_hypervisor_status_request(request_id);
    if (!coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_hypervisor_status_frame(frame_body);
        })) {
        hypervisor_status_loading_ = false;
    }
}

QString ServiceClient::errorText() const {
    return error_code_.isEmpty() ? QString{} : localize_message_code(error_code_);
}

bool ServiceClient::repositoryConfigured() const noexcept { return repository_configured_; }
bool ServiceClient::repositoryLoading() const noexcept { return repository_loading_; }
QString ServiceClient::repositoryUuid() const { return repository_uuid_; }

QString ServiceClient::repositoryStatusText() const {
    if (!connected()) {
        //% "Waiting for Service"
        return qtTrId("aegra.repository.status.waiting_service");
    }
    if (repository_loading_) {
        //% "Reading catalog"
        return qtTrId("aegra.repository.status.loading");
    }
    if (!repository_error_code_.isEmpty()) {
        //% "Catalog read failed"
        return qtTrId("aegra.repository.status.read_failed");
    }
    if (repository_configured_) {
        //% "Catalog available"
        return qtTrId("aegra.repository.status.catalog_ready");
    }
    //% "Not configured"
    return qtTrId("aegra.repository.status.not_configured");
}

QString ServiceClient::repositoryErrorText() const {
    return repository_error_code_.isEmpty() ? QString{}
                                            : localize_message_code(repository_error_code_);
}

RecoveryPointModel* ServiceClient::recoveryPoints() noexcept { return &recovery_points_; }
int ServiceClient::recoveryPointCount() const { return recovery_points_.rowCount(); }
JobModel* ServiceClient::jobs() noexcept { return &jobs_; }
bool ServiceClient::jobsLoading() const noexcept { return jobs_loading_; }
bool ServiceClient::jobListAvailable() const noexcept { return job_list_available_; }

QString ServiceClient::jobsErrorText() const {
    return jobs_error_code_.isEmpty() ? QString{} : localize_message_code(jobs_error_code_);
}

JobModel* ServiceClient::taskLog() noexcept { return &task_log_; }
bool ServiceClient::taskLogLoading() const noexcept { return task_log_loading_; }
bool ServiceClient::taskLogHasMore() const noexcept { return task_log_next_token_.has_value(); }

QString ServiceClient::taskLogErrorText() const {
    return task_log_error_code_.isEmpty() ? QString{} : localize_message_code(task_log_error_code_);
}

bool ServiceClient::serviceSettingsAvailable() const noexcept {
    return service_settings_available_;
}

bool ServiceClient::serviceSettingsLoading() const noexcept { return service_settings_loading_; }

bool ServiceClient::serviceSettingsBusy() const noexcept { return service_settings_busy_; }

int ServiceClient::jobRetentionMonths() const noexcept { return job_retention_months_; }

QString ServiceClient::serviceSettingsErrorText() const {
    return service_settings_error_code_.isEmpty()
               ? QString{}
               : localize_message_code(service_settings_error_code_);
}

void ServiceClient::refreshServiceSettings() {
    if (state_ != State::kReady || !service_settings_available_ || service_settings_loading_ ||
        service_settings_busy_) {
        return;
    }
    service_settings_loading_ = true;
    service_settings_error_code_.clear();
    emit serviceSettingsChanged();
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    service_settings_request_id_ = request_id;
    const auto body = encode_get_service_settings_request(request_id);
    if (!coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_get_service_settings_frame(frame_body);
        })) {
        service_settings_loading_ = false;
        service_settings_error_code_ = QStringLiteral("service.send_failed");
        emit serviceSettingsChanged();
    }
}

bool ServiceClient::setJobRetentionMonths(const int months) {
    if (state_ != State::kReady || !service_settings_available_ || service_settings_busy_ ||
        service_settings_loading_) {
        return false;
    }
    if (months != kJobRetentionMonths1 && months != kJobRetentionMonths3 &&
        months != kJobRetentionMonths6) {
        return false;
    }
    if (months == job_retention_months_) {
        return true;
    }
    service_settings_busy_ = true;
    service_settings_error_code_.clear();
    pending_job_retention_months_ = months;
    service_settings_update_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit serviceSettingsChanged();
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    service_settings_update_request_id_ = request_id;
    const auto body = encode_update_service_settings_request(
        request_id, service_settings_update_idempotency_key_, months);
    if (!coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_update_service_settings_frame(frame_body);
        })) {
        service_settings_busy_ = false;
        service_settings_error_code_ = QStringLiteral("service.send_failed");
        emit serviceSettingsChanged();
        return false;
    }
    return true;
}

SourceInventoryModel* ServiceClient::sources() noexcept { return &sources_; }
bool ServiceClient::inventoryLoading() const noexcept { return inventory_loading_; }
bool ServiceClient::inventoryAvailable() const noexcept { return inventory_available_; }

QString ServiceClient::inventoryErrorText() const {
    return inventory_error_code_.isEmpty() ? QString{}
                                           : localize_message_code(inventory_error_code_);
}

RepositoryConnectionModel* ServiceClient::connections() noexcept { return &connections_; }
bool ServiceClient::connectionsLoading() const noexcept { return connections_loading_; }
bool ServiceClient::repositoryRefreshRunning() const noexcept {
    return repository_refresh_running_;
}
bool ServiceClient::connectionsAvailable() const noexcept { return connections_available_; }

QString ServiceClient::connectionsErrorText() const {
    return connections_error_code_.isEmpty() ? QString{}
                                             : localize_message_code(connections_error_code_);
}

bool ServiceClient::backupStartAvailable() const noexcept { return backup_start_available_; }
bool ServiceClient::restoreStartAvailable() const noexcept {
    return restore_start_available_ && restore_preflight_available_;
}
bool ServiceClient::ntfsShrinkAvailable() const noexcept { return ntfs_shrink_available_; }
bool ServiceClient::restoreCommandBusy() const noexcept { return restore_command_busy_; }

QObject* ServiceClient::peRestore() const noexcept { return pe_restore_.get(); }
bool ServiceClient::mountStartAvailable() const noexcept {
    return mount_start_available_ && mount_list_available_;
}
bool ServiceClient::mountListAvailable() const noexcept { return mount_list_available_; }
bool ServiceClient::mountCommandBusy() const noexcept { return mount_command_busy_; }
bool ServiceClient::mountSessionsLoading() const noexcept { return mount_sessions_loading_; }
QVariantList ServiceClient::mountSessions() const { return mount_sessions_; }
QString ServiceClient::mountSessionsErrorText() const {
    return mount_sessions_error_code_.isEmpty() ? QString{}
                                                : localize_message_code(mount_sessions_error_code_);
}
QString ServiceClient::mountCommandErrorText() const {
    return mount_command_error_code_.isEmpty() ? QString{}
                                               : localize_message_code(mount_command_error_code_);
}
bool ServiceClient::jobCancelAvailable() const noexcept { return job_cancel_available_; }
bool ServiceClient::backupCommandBusy() const noexcept { return backup_command_busy_; }
bool ServiceClient::cancelCommandBusy() const noexcept { return cancel_command_busy_; }

QString ServiceClient::backupCommandErrorText() const {
    return backup_command_error_code_.isEmpty() ? QString{}
                                                : localize_message_code(backup_command_error_code_);
}

QString ServiceClient::activeBackupJobId() const { return active_backup_job_id_; }
QString ServiceClient::activeBackupStateText() const { return active_backup_state_text_; }
int ServiceClient::activeBackupProgressPercent() const noexcept {
    return active_backup_progress_percent_;
}
bool ServiceClient::activeBackupProgressVisible() const noexcept {
    return active_backup_progress_visible_;
}
QString ServiceClient::activeBackupMessageText() const { return active_backup_message_text_; }
bool ServiceClient::activeBackupTerminal() const noexcept { return active_backup_terminal_; }
bool ServiceClient::activeBackupCancellable() const noexcept { return active_backup_cancellable_; }

bool ServiceClient::splashVisible() const noexcept { return !first_ready_seen_; }
bool ServiceClient::splashBusy() const noexcept {
    return !first_ready_seen_ &&
           (state_ == State::kConnecting ||
            (splash_connect_timer_ != nullptr && splash_connect_timer_->isActive())) &&
           !splash_error_;
}

QString ServiceClient::splashStatusText() const {
    if (first_ready_seen_) {
        return {};
    }
    if (splash_error_) {
        //% "Unable to connect to the backup server"
        return qtTrId("aegra.splash.status.failed");
    }
    //% "Loading data..."
    return qtTrId("aegra.splash.status.connecting");
}

QString ServiceClient::splashErrorText() const { return splash_error_ ? errorText() : QString{}; }

bool ServiceClient::toastVisible() const noexcept { return toast_visible_; }
QString ServiceClient::toastText() const { return toast_text_; }
bool ServiceClient::toastIsError() const noexcept { return toast_is_error_; }
bool ServiceClient::hasUnreadEvents() const noexcept { return has_unread_events_; }
bool ServiceClient::globalLoading() const noexcept {
    // After splash: full-window overlay while page catalog queries run (menu switch reload),
    // matching old AegraImage Main.qml appLoading.
    if (state_ != State::kReady || !first_ready_seen_) {
        return false;
    }
    return recovery_point_layout_loading_ || jobs_loading_ || inventory_loading_ ||
           (connections_loading_ && !repository_refresh_running_) || schedules_loading_ ||
           (repository_command_busy_ && !repository_refresh_running_) || backup_command_busy_ ||
           cancel_command_busy_ || (schedule_command_busy_ && !schedule_enable_patch_active_) ||
           mount_command_busy_;
}

bool ServiceClient::hasCapability(const QString& capability) const {
    return capabilities_.contains(capability);
}

void ServiceClient::reconnect() {
    splash_error_ = false;
    pending_splash_error_code_.clear();
    if (!first_ready_seen_) {
        transport_->set_reconnect_delay_milliseconds(600);
        transport_->set_auto_reconnect_enabled(true);
        splash_connect_timer_->start(3'000);
    } else {
        transport_->set_reconnect_delay_milliseconds(
            IpcFrameTransport::kDefaultReconnectDelayMilliseconds);
        transport_->set_auto_reconnect_enabled(true);
    }
    set_state(State::kConnecting);
    update_splash_for_state();
    transport_->connect_to_service();
}

void ServiceClient::refreshRepository() {
    if (state_ != State::kReady || repository_loading_ ||
        (connections_available_ && selected_repository_connection_id_.isEmpty()) ||
        (!repository_request_id_.isEmpty() &&
         coordinator_->has_pending_request(repository_request_id_))) {
        return;
    }
    start_repository_query();
}

void ServiceClient::refreshJobs() {
    if (state_ != State::kReady || !job_list_available_ || jobs_loading_ ||
        (!job_request_id_.isEmpty() && coordinator_->has_pending_request(job_request_id_))) {
        return;
    }
    start_job_query();
}

void ServiceClient::refreshInventory() {
    if (state_ != State::kReady || !inventory_available_ || inventory_loading_ ||
        (!inventory_request_id_.isEmpty() &&
         coordinator_->has_pending_request(inventory_request_id_))) {
        return;
    }
    start_inventory_query();
}

void ServiceClient::refreshConnections() {
    if (state_ != State::kReady || !connections_available_ || connections_loading_ ||
        (!connection_request_id_.isEmpty() &&
         coordinator_->has_pending_request(connection_request_id_))) {
        return;
    }
    start_connection_query();
}

void ServiceClient::refreshRepositoryConnections() {
    if (state_ != State::kReady || !connections_available_ || connections_loading_ ||
        repository_command_busy_ || repository_refresh_running_) {
        return;
    }
    repository_refresh_queue_ = connections_.connection_ids();
    if (repository_refresh_queue_.isEmpty()) {
        return;
    }
    repository_refresh_running_ = true;
    repository_refresh_waiting_for_snapshot_ = false;
    repository_refresh_current_id_.clear();
    connections_error_code_.clear();
    emit connectionsChanged();
    begin_next_repository_refresh();
}

void ServiceClient::dismissToast() {
    if (!toast_visible_) {
        return;
    }
    toast_visible_ = false;
    toast_text_.clear();
    toast_is_error_ = false;
    emit toastChanged();
}

void ServiceClient::markEventsRead() {
    if (!has_unread_events_) {
        return;
    }
    has_unread_events_ = false;
    emit unreadEventsChanged();
}

void ServiceClient::on_transport_connected() {
    if (splash_connect_timer_ != nullptr) {
        splash_connect_timer_->stop();
    }
    pending_splash_error_code_.clear();
    handshake_complete_ = false;
    splash_error_ = false;
    if (state_ != State::kConnecting) {
        set_state(State::kConnecting);
    } else {
        update_splash_for_state();
    }
    send_service_info_request();
}

void ServiceClient::on_transport_disconnected() {
    if (state_ == State::kDisconnected) {
        return;
    }
    if (!first_ready_seen_ && splash_connect_timer_ != nullptr &&
        splash_connect_timer_->isActive()) {
        pending_splash_error_code_ = QStringLiteral("service.disconnected");
        return;
    }
    set_state(State::kDisconnected, QStringLiteral("service.disconnected"));
}

void ServiceClient::on_transport_error(const QString& message_code) {
    if (!first_ready_seen_ && splash_connect_timer_ != nullptr &&
        splash_connect_timer_->isActive()) {
        pending_splash_error_code_ = message_code;
        return;
    }
    set_state(State::kDisconnected, message_code);
}

void ServiceClient::on_request_failed(const QString& message_code) {
    // Handshake-level failures drop the transport. Query failures after Ready must not tear down
    // the whole Service session (matches desktop.md: catalog errors stay on repository status).
    const bool transport_level = message_code == QLatin1String("service.protocol_invalid") ||
                                 message_code == QLatin1String("service.send_failed") ||
                                 message_code == QLatin1String("service.disconnected");
    if (!handshake_complete_ || transport_level) {
        if (!first_ready_seen_ && splash_connect_timer_ != nullptr &&
            splash_connect_timer_->isActive()) {
            pending_splash_error_code_ = message_code;
            return;
        }
        // After Ready, soft-fail in-flight domain queries instead of IPC reconnect storms.
        // A request timeout is correlated by ServiceRequestCoordinator and never reaches this
        // transport-level path. Protocol-invalid domain responses keep the live pipe.
        const bool domain_inflight =
            repository_loading_ || recovery_point_layout_loading_ || jobs_loading_ ||
            task_log_loading_ || inventory_loading_ || connections_loading_ || schedules_loading_ ||
            mount_sessions_loading_ || repository_command_busy_ || schedule_command_busy_ ||
            backup_command_busy_ || cancel_command_busy_ || mount_command_busy_;
        if (handshake_complete_ && first_ready_seen_ && domain_inflight &&
            (message_code == QLatin1String("service.protocol_invalid") ||
             message_code == QLatin1String("service.request_timeout"))) {
            if (repository_loading_) {
                finish_repository_failure(QStringLiteral("repository.query_failed"));
            }
            if (recovery_point_layout_loading_) {
                finish_recovery_point_layout_failure(
                    QStringLiteral("recovery_point.layout_failed"));
            }
            if (jobs_loading_) {
                finish_job_failure(QStringLiteral("job.query_failed"));
            }
            if (task_log_loading_) {
                finish_task_log_failure(QStringLiteral("job.query_failed"));
            }
            if (inventory_loading_) {
                finish_inventory_failure(QStringLiteral("inventory.query_failed"));
            }
            if (connections_loading_) {
                finish_connection_failure(QStringLiteral("connection.query_failed"));
            }
            if (schedules_loading_) {
                finish_schedule_failure(QStringLiteral("schedule.query_failed"));
            }
            if (mount_sessions_loading_) {
                finish_mount_list_failure(QStringLiteral("mount.list_failed"));
            }
            if (repository_command_busy_) {
                finish_repository_command_failure(QStringLiteral("service.request_failed"));
            }
            if (backup_command_busy_) {
                finish_backup_command_failure(QStringLiteral("backup.command_failed"));
            }
            if (cancel_command_busy_) {
                finish_cancel_command_failure(QStringLiteral("job.cancel_failed"));
            }
            if (schedule_command_busy_) {
                finish_schedule_command_failure(QStringLiteral("schedule.command_failed"));
            }
            if (mount_command_busy_) {
                finish_mount_command_failure(QStringLiteral("mount.command_failed"));
            }
            // protocol_invalid: keep the live pipe; only soft-fail the domain request.
            if (message_code == QLatin1String("service.protocol_invalid")) {
                return;
            }
        }
        set_state(State::kDisconnected, message_code);
        transport_->disconnect_from_service();
        if (first_ready_seen_) {
            transport_->set_auto_reconnect_enabled(true);
            // Backoff reconnect — never zero-delay reconnect loops after Ready.
            transport_->schedule_reconnect_with_backoff();
        }
        return;
    }
    if (repository_loading_) {
        finish_repository_failure(QStringLiteral("repository.query_failed"));
    }
    if (recovery_point_layout_loading_) {
        finish_recovery_point_layout_failure(QStringLiteral("recovery_point.layout_failed"));
    }
    if (jobs_loading_) {
        finish_job_failure(QStringLiteral("job.query_failed"));
    }
    if (task_log_loading_) {
        finish_task_log_failure(QStringLiteral("job.query_failed"));
    }
    if (inventory_loading_) {
        finish_inventory_failure(QStringLiteral("inventory.query_failed"));
    }
    if (connections_loading_) {
        finish_connection_failure(QStringLiteral("connection.query_failed"));
    }
    if (schedules_loading_) {
        finish_schedule_failure(QStringLiteral("schedule.query_failed"));
    }
    if (mount_sessions_loading_) {
        finish_mount_list_failure(QStringLiteral("mount.list_failed"));
    }
    if (repository_command_busy_) {
        finish_repository_command_failure(QStringLiteral("service.request_failed"));
    }
    if (repository_directories_loading_) {
        finish_repository_directories_failure(QStringLiteral("service.request_failed"));
    }
    if (backup_command_busy_) {
        finish_backup_command_failure(QStringLiteral("backup.command_failed"));
    }
    if (cancel_command_busy_) {
        finish_cancel_command_failure(QStringLiteral("job.cancel_failed"));
    }
    if (schedule_command_busy_) {
        finish_schedule_command_failure(QStringLiteral("schedule.command_failed"));
    }
    if (mount_command_busy_) {
        finish_mount_command_failure(QStringLiteral("mount.command_failed"));
    }
}

void ServiceClient::on_locale_changed() {
    update_format_locale();
    recovery_points_.retranslate();
    jobs_.retranslate();
    task_log_.retranslate();
    sources_.retranslate();
    connections_.retranslate();
    update_active_backup_observe();
    emit stateChanged();
    emit repositoryChanged();
    emit jobsChanged();
    emit taskLogChanged();
    emit inventoryChanged();
    emit connectionsChanged();
    emit repositoryCommandChanged();
    emit backupCommandChanged();
    emit splashChanged();
}

void ServiceClient::on_job_poll_tick() {
    if (state_ != State::kReady || !job_list_available_) {
        update_job_polling();
        return;
    }
    // Non-overlapping: skip while a job query is already in flight.
    if (jobs_loading_ ||
        (!job_request_id_.isEmpty() && coordinator_->has_pending_request(job_request_id_))) {
        return;
    }
    // Prefer terminal merge over another active snapshot: a sub-second restore leaves the
    // active set while JobModel still holds the last Running percent.
    if (pending_terminal_job_sync_) {
        start_terminal_job_seed();
        return;
    }
    if (!jobs_.has_active_jobs()) {
        const auto now_ms = QDateTime::currentMSecsSinceEpoch();
        if (now_ms >= job_chain_watch_deadline_ms_) {
            update_job_polling();
            return;
        }
        // Grace window: sample slower than the active cadence while waiting for
        // the Service to submit the next post-backup job.
        if (now_ms - last_grace_job_query_ms_ < kGraceJobQueryIntervalMilliseconds) {
            return;
        }
        last_grace_job_query_ms_ = now_ms;
    }
    start_job_query();
}

void ServiceClient::send_service_info_request() {
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto body = encode_service_info_request(request_id);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_service_info_frame(frame_body);
        });
    if (!started) {
        set_state(State::kDisconnected, QStringLiteral("service.send_failed"));
    }
}

JobListQuery
ServiceClient::make_active_job_query(const std::optional<QString>& continuation_token) const {
    JobListQuery query;
    query.scope = kJobListScopeActive;
    query.continuation_token = continuation_token;
    query.maximum_results = kJobPageSize;
    return query;
}

JobListQuery ServiceClient::make_terminal_seed_query() const {
    JobListQuery query;
    query.scope = kJobListScopeTerminal;
    query.maximum_results = kJobPageSize;
    if (awaiting_terminal_job_ids_.isEmpty()) {
        return query;
    }
    qint64 earliest = 0;
    for (const auto& job_id : awaiting_terminal_job_ids_) {
        const auto job = jobs_.find_job(job_id);
        if (!job) {
            continue;
        }
        if (earliest == 0 || job->created_utc_ms < earliest) {
            earliest = job->created_utc_ms;
        }
    }
    if (earliest > 0) {
        query.from_utc_ms = (std::max)(earliest - 60'000, qint64{0});
    }
    return query;
}

JobListQuery
ServiceClient::make_task_log_query(const std::optional<QString>& continuation_token) const {
    auto query = task_log_query_;
    query.continuation_token = continuation_token;
    return query;
}

void ServiceClient::start_job_query() {
    if (!job_list_available_ || task_log_loading_) {
        return;
    }
    if (jobs_loading_ ||
        (!job_request_id_.isEmpty() && coordinator_->has_pending_request(job_request_id_))) {
        return;
    }
    job_query_purpose_ = JobQueryPurpose::kActive;
    jobs_error_code_.clear();
    jobs_loading_ = true;
    pending_jobs_.clear();
    job_requested_token_.reset();
    emit jobsChanged();
    emit loadingChanged();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job_request_id_ = request_id;
    const auto body = encode_job_list_request(request_id, make_active_job_query(std::nullopt));
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_job_list_frame(frame_body);
        });
    if (!started) {
        finish_job_failure(QStringLiteral("job.query_failed"));
    }
}

void ServiceClient::start_terminal_job_seed() {
    if (!job_list_available_ || task_log_loading_) {
        return;
    }
    if (jobs_loading_ ||
        (!job_request_id_.isEmpty() && coordinator_->has_pending_request(job_request_id_))) {
        return;
    }
    job_query_purpose_ = JobQueryPurpose::kTerminalSeed;
    jobs_error_code_.clear();
    jobs_loading_ = true;
    pending_jobs_.clear();
    job_requested_token_.reset();
    emit jobsChanged();
    emit loadingChanged();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job_request_id_ = request_id;
    const auto body = encode_job_list_request(request_id, make_terminal_seed_query());
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_job_list_frame(frame_body);
        });
    if (!started) {
        finish_job_failure(QStringLiteral("job.query_failed"));
    }
}

void ServiceClient::start_task_log_query(const bool append) {
    if (!job_list_available_ || jobs_loading_) {
        return;
    }
    if (task_log_loading_ || (!task_log_request_id_.isEmpty() &&
                              coordinator_->has_pending_request(task_log_request_id_))) {
        return;
    }
    if (!append) {
        pending_task_log_.clear();
        task_log_requested_token_.reset();
        task_log_next_token_.reset();
        task_log_append_ = false;
    } else if (!task_log_next_token_) {
        return;
    } else {
        task_log_append_ = true;
        pending_task_log_.clear();
    }
    job_query_purpose_ = JobQueryPurpose::kTaskLog;
    task_log_error_code_.clear();
    task_log_loading_ = true;
    emit taskLogChanged();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    task_log_request_id_ = request_id;
    const auto token = append ? task_log_next_token_ : std::nullopt;
    const auto body = encode_job_list_request(request_id, make_task_log_query(token));
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_job_list_frame(frame_body);
        });
    if (!started) {
        finish_task_log_failure(QStringLiteral("job.query_failed"));
    }
}

void ServiceClient::refreshTaskLog(const int time_index, const int type_index,
                                   const int status_index) {
    JobListQuery query;
    query.scope = kJobListScopeTerminal;
    query.maximum_results = kJobPageSize;
    const auto now_ms = QDateTime::currentMSecsSinceEpoch();
    if (time_index == 1) {
        query.from_utc_ms = now_ms - 24LL * 60 * 60 * 1000;
    } else if (time_index == 2) {
        query.from_utc_ms = now_ms - 7LL * 24 * 60 * 60 * 1000;
    } else if (time_index == 3) {
        query.from_utc_ms = now_ms - 30LL * 24 * 60 * 60 * 1000;
    }
    if (type_index == 1) {
        query.operation = 1; // backup
    } else if (type_index == 2) {
        query.operation = 2; // restore
    } else if (type_index == 3) {
        query.operation = 3; // verify
    } else if (type_index == 4) {
        query.operation = 5; // boot check
    }
    if (status_index == 1) {
        query.state = 4; // succeeded
    } else if (status_index == 2) {
        query.state = 5; // failed
    } else if (status_index == 3) {
        query.state = 6; // cancelled
    }
    task_log_query_ = std::move(query);
    start_task_log_query(false);
}

void ServiceClient::loadMoreTaskLog() {
    if (!task_log_next_token_ || task_log_loading_) {
        return;
    }
    start_task_log_query(true);
}

RequestDisposition ServiceClient::handle_service_info_frame(const QByteArray& body) {
    QJsonObject root;
    if (!parse_response_root(body, extract_response_request_id(body), root)) {
        return RequestDisposition::kProtocolError;
    }
    ServiceInfo service;
    if (!parse_service_info_response(root, service) ||
        !service.capabilities.contains(QStringLiteral("repository.list"))) {
        return RequestDisposition::kProtocolError;
    }
    service_version_ = std::move(service.version);
    api_version_ = kServiceApiVersion;
    capabilities_ = std::move(service.capabilities);
    job_list_available_ = capabilities_.contains(QStringLiteral("job.list"));
    service_settings_available_ = capabilities_.contains(QStringLiteral("service.settings"));
    inventory_available_ = capabilities_.contains(QStringLiteral("source.inventory"));
    connections_available_ = capabilities_.contains(QStringLiteral("repository.connection"));
    schedules_available_ = capabilities_.contains(QStringLiteral("schedule"));
    file_browse_available_ = capabilities_.contains(QStringLiteral("file.browse"));
    file_recover_browse_available_ = capabilities_.contains(QStringLiteral("file.recover_browse"));
    file_restore_available_ = capabilities_.contains(QStringLiteral("file.restore"));
    backup_start_available_ = capabilities_.contains(QStringLiteral("backup.start"));
    restore_preflight_available_ = capabilities_.contains(QStringLiteral("restore.preflight"));
    restore_start_available_ = capabilities_.contains(QStringLiteral("restore.start"));
    ntfs_shrink_available_ = capabilities_.contains(QStringLiteral("restore.ntfs_shrink.v1"));
    pe_restore_->set_available(capabilities_.contains(QStringLiteral("restore.pe.arm")));
    mount_list_available_ = capabilities_.contains(QStringLiteral("mount.list"));
    mount_start_available_ = capabilities_.contains(QStringLiteral("mount.start"));
    mount_unmount_available_ = capabilities_.contains(QStringLiteral("mount.unmount"));
    job_cancel_available_ = capabilities_.contains(QStringLiteral("job.cancel"));
    handshake_complete_ = true;
    QTimer::singleShot(0, this, [this]() {
        if (!handshake_complete_) {
            return;
        }
        set_state(State::kReady);
        first_ready_seen_ = true;
        splash_error_ = false;
        update_splash_for_state();
        if (job_list_available_) {
            // Terminal seed first (schedule status / toast baseline), then active poll.
            start_terminal_job_seed();
        }
        if (inventory_available_) {
            start_inventory_query();
        }
        if (pe_restore_->available()) {
            pe_restore_->refresh();
        }
        if (connections_available_) {
            start_connection_query();
        }
        if (schedules_available_) {
            start_schedule_query();
        }
        if (mount_list_available_) {
            start_mount_session_query();
        }
        if (service_settings_available_) {
            refreshServiceSettings();
        }
        start_hypervisor_status_query();
    });
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_job_list_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    const auto purpose = job_query_purpose_;
    if (is_job_failure_response(root)) {
        if (purpose == JobQueryPurpose::kTaskLog) {
            finish_task_log_failure(QStringLiteral("job.query_failed"));
        } else {
            finish_job_failure(QStringLiteral("job.query_failed"));
        }
        return RequestDisposition::kFinished;
    }
    JobPage page;
    if (!parse_job_list_response(root, page)) {
        return RequestDisposition::kProtocolError;
    }

    auto& pending = purpose == JobQueryPurpose::kTaskLog ? pending_task_log_ : pending_jobs_;
    auto& requested_token =
        purpose == JobQueryPurpose::kTaskLog ? task_log_requested_token_ : job_requested_token_;

    if ((page.continuation_token && page.continuation_token == requested_token) ||
        pending.size() + page.items.size() > kMaximumJobs) {
        return RequestDisposition::kProtocolError;
    }
    QSet<QString> seen_ids;
    for (const auto& existing : pending) {
        seen_ids.insert(existing.toMap().value(QStringLiteral("jobId")).toString());
    }
    for (auto& item : page.items) {
        const auto job_id = item.toMap().value(QStringLiteral("jobId")).toString();
        if (seen_ids.contains(job_id)) {
            return RequestDisposition::kProtocolError;
        }
        seen_ids.insert(job_id);
        pending.push_back(std::move(item));
    }

    // Active / Task Log: follow continuation. Terminal seed: one page only (schedule status).
    if (page.continuation_token && purpose != JobQueryPurpose::kTerminalSeed) {
        requested_token = page.continuation_token;
        const auto next_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        JobListQuery next_query;
        if (purpose == JobQueryPurpose::kActive) {
            next_query = make_active_job_query(requested_token);
        } else if (purpose == JobQueryPurpose::kTaskLog) {
            next_query = make_task_log_query(requested_token);
        }
        const auto next_body = encode_job_list_request(next_id, next_query);
        if (!coordinator_->continue_request(request_id, next_id, next_body)) {
            return RequestDisposition::kProtocolError;
        }
        if (purpose == JobQueryPurpose::kTaskLog) {
            task_log_request_id_ = next_id;
        } else {
            job_request_id_ = next_id;
        }
        return RequestDisposition::kContinue;
    }

    auto rows = jobs_from_variant_list(pending);
    for (auto& row : rows) {
        enrich_job_row(row);
    }

    if (purpose == JobQueryPurpose::kTaskLog) {
        task_log_next_token_ = page.continuation_token;
        if (task_log_append_) {
            for (auto& row : rows) {
                task_log_.upsert_job(std::move(row));
            }
        } else {
            task_log_.set_rows(std::move(rows));
        }
        pending_task_log_.clear();
        task_log_loading_ = false;
        task_log_append_ = false;
        task_log_request_id_.clear();
        task_log_requested_token_.reset();
        emit taskLogChanged();
        return RequestDisposition::kFinished;
    }

    if (purpose == JobQueryPurpose::kTerminalSeed) {
        apply_terminal_job_seed(std::move(rows));
        pending_jobs_.clear();
        jobs_loading_ = false;
        job_request_id_.clear();
        job_requested_token_.reset();
        emit jobsChanged();
        emit loadingChanged();
        update_job_polling();
        update_active_backup_observe();
        // After the first seed, or after a completed job was merged, refresh active jobs.
        // If a vanished job is still unresolved, the poll tick retries terminal seed.
        if (!pending_terminal_job_sync_) {
            start_job_query();
        }
        return RequestDisposition::kFinished;
    }

    apply_active_job_snapshot(std::move(rows));
    pending_jobs_.clear();
    jobs_loading_ = false;
    job_request_id_.clear();
    job_requested_token_.reset();
    emit jobsChanged();
    emit loadingChanged();
    update_job_polling();
    update_active_backup_observe();
    if (pending_terminal_job_sync_) {
        QTimer::singleShot(0, this, [this]() {
            if (state_ != State::kReady || !job_list_available_ || jobs_loading_ ||
                task_log_loading_ || !pending_terminal_job_sync_) {
                update_job_polling();
                return;
            }
            start_terminal_job_seed();
        });
    }
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_job_failure(const QString& message_code) {
    pending_jobs_.clear();
    job_requested_token_.reset();
    jobs_loading_ = false;
    job_request_id_.clear();
    jobs_error_code_ = message_code;
    emit jobsChanged();
    emit loadingChanged();
    update_job_polling();
}

void ServiceClient::finish_task_log_failure(const QString& message_code) {
    pending_task_log_.clear();
    task_log_requested_token_.reset();
    task_log_loading_ = false;
    task_log_append_ = false;
    task_log_request_id_.clear();
    task_log_error_code_ = message_code;
    emit taskLogChanged();
}

void ServiceClient::reset_task_log() {
    task_log_.clear();
    pending_task_log_.clear();
    task_log_requested_token_.reset();
    task_log_next_token_.reset();
    task_log_loading_ = false;
    task_log_error_code_.clear();
    task_log_request_id_.clear();
    task_log_query_ = JobListQuery{};
    task_log_query_.scope = kJobListScopeTerminal;
    emit taskLogChanged();
}

RequestDisposition ServiceClient::handle_get_service_settings_frame(const QByteArray& body) {
    QJsonObject root;
    if (!parse_response_root(body, extract_response_request_id(body), root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_service_settings_failure_response(root)) {
        finish_service_settings_failure(root.value(QStringLiteral("message_code")).toString());
        return RequestDisposition::kFinished;
    }
    ServiceSettings settings;
    if (!parse_service_settings_response(root, settings)) {
        // Fail safe: an unexpected-but-addressed reply must not leave the
        // settings UI disabled forever behind a stuck loading flag.
        finish_service_settings_failure(QStringLiteral("service.settings_failed"));
        return RequestDisposition::kProtocolError;
    }
    job_retention_months_ = settings.job_retention_months;
    service_settings_loading_ = false;
    service_settings_error_code_.clear();
    service_settings_request_id_.clear();
    emit serviceSettingsChanged();
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_update_service_settings_frame(const QByteArray& body) {
    QJsonObject root;
    if (!parse_response_root(body, extract_response_request_id(body), root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, kUpdateServiceSettingsRequestKind)) {
        finish_service_settings_failure(root.value(QStringLiteral("message_code")).toString());
        return RequestDisposition::kFinished;
    }
    CommandAck ack;
    if (!parse_command_ack_response(root, kUpdateServiceSettingsRequestKind, ack)) {
        // Fail safe: an unexpected-but-addressed reply must not leave the
        // settings UI disabled forever behind a stuck busy flag.
        finish_service_settings_failure(QStringLiteral("service.settings_update_failed"));
        return RequestDisposition::kProtocolError;
    }
    job_retention_months_ = pending_job_retention_months_;
    service_settings_busy_ = false;
    service_settings_error_code_.clear();
    service_settings_update_request_id_.clear();
    service_settings_update_idempotency_key_.clear();
    emit serviceSettingsChanged();
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_hypervisor_status_frame(const QByteArray& body) {
    QJsonObject root;
    if (!parse_response_root(body, extract_response_request_id(body), root)) {
        return RequestDisposition::kProtocolError;
    }
    hypervisor_status_loading_ = false;
    hypervisor_status_request_id_.clear();
    if (is_boot_check_hypervisor_status_failure_response(root)) {
        // Status stays at its previous snapshot; the wizard still renders the
        // installed flags from capabilities.
        emit hypervisorStatusChanged();
        return RequestDisposition::kFinished;
    }
    QList<BootCheckHypervisorStatus> statuses;
    if (!parse_boot_check_hypervisor_status_response(root, statuses)) {
        emit hypervisorStatusChanged();
        return RequestDisposition::kProtocolError;
    }
    hypervisor_status_ = std::move(statuses);
    const bool probing =
        std::ranges::any_of(hypervisor_status_, [](const BootCheckHypervisorStatus& status) {
            return status.probe_state == kBootCheckProbeStateProbing;
        });
    if (probing) {
        hypervisor_poll_timer_->start();
    } else {
        hypervisor_poll_timer_->stop();
    }
    emit hypervisorStatusChanged();
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_hypervisor_refresh_frame(const QByteArray& body) {
    QJsonObject root;
    if (!parse_response_root(body, extract_response_request_id(body), root)) {
        return RequestDisposition::kProtocolError;
    }
    hypervisor_refresh_busy_ = false;
    hypervisor_refresh_request_id_.clear();
    CommandAck ack;
    if (!is_command_failure_response(root, kRefreshBootCheckHypervisorStatusRequestKind) &&
        parse_command_ack_response(root, kRefreshBootCheckHypervisorStatusRequestKind, ack)) {
        // Probe accepted: pull the status now so the UI flips into "probing".
        start_hypervisor_status_query();
    }
    emit hypervisorStatusChanged();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_service_settings_failure(const QString& message_code) {
    service_settings_loading_ = false;
    service_settings_busy_ = false;
    service_settings_error_code_ =
        message_code.isEmpty() ? QStringLiteral("service.settings_failed") : message_code;
    service_settings_request_id_.clear();
    service_settings_update_request_id_.clear();
    service_settings_update_idempotency_key_.clear();
    emit serviceSettingsChanged();
}

void ServiceClient::reset_service_settings() {
    service_settings_available_ = false;
    service_settings_loading_ = false;
    service_settings_busy_ = false;
    job_retention_months_ = kDefaultJobRetentionMonths;
    pending_job_retention_months_ = kDefaultJobRetentionMonths;
    service_settings_error_code_.clear();
    service_settings_request_id_.clear();
    service_settings_update_request_id_.clear();
    service_settings_update_idempotency_key_.clear();
    emit serviceSettingsChanged();
}

void ServiceClient::reset_jobs() {
    jobs_.clear();
    pending_jobs_.clear();
    job_requested_token_.reset();
    jobs_loading_ = false;
    jobs_error_code_.clear();
    job_request_id_.clear();
    jobs_baseline_seeded_ = false;
    pending_terminal_job_sync_ = false;
    awaiting_terminal_job_ids_.clear();
    toasted_job_keys_.clear();
    job_poll_timer_->stop();
    reset_task_log();
    emit jobsChanged();
    emit loadingChanged();
    update_active_backup_observe();
}

void ServiceClient::reset_file_models() {
    file_browse_sources_.clear();
    file_restore_targets_.clear();
    file_recover_entries_.clear();
    file_browse_request_id_.clear();
    file_browse_parent_token_.clear();
    file_target_browse_request_id_.clear();
    file_target_browse_parent_token_.clear();
    file_recover_request_id_.clear();
    file_recover_parent_entry_id_.clear();
    file_recover_archive_password_.clear();
    file_restore_entry_ids_.clear();
    file_restore_target_token_.clear();
    file_restore_conflict_policy_ = kFileConflictPolicyFail;
    file_restore_security_ = true;
}

void ServiceClient::set_state(const State state, QString error_code) {
    const bool same_state = (state_ == state);
    const bool same_error = (error_code_ == error_code);
    if (same_state && same_error) {
        if (!first_ready_seen_ && state == State::kDisconnected && !splash_error_) {
            if (splash_connect_timer_ == nullptr || !splash_connect_timer_->isActive()) {
                splash_error_ = true;
                transport_->set_auto_reconnect_enabled(false);
                update_splash_for_state();
            }
        } else if (first_ready_seen_ && state == State::kDisconnected) {
            // Keep trying after main UI is up (Service may start later).
            transport_->ensure_reconnect_scheduled();
        }
        return;
    }

    const State previous = state_;
    state_ = state;
    error_code_ = std::move(error_code);

    if (state != State::kReady) {
        if (previous == State::kReady) {
            // Dropped after a successful session. Disable commands but retain the last complete
            // Service snapshots so a transient disconnect does not blank every page.
            service_version_.clear();
            api_version_ = 0;
            capabilities_.clear();
            job_list_available_ = false;
            inventory_available_ = false;
            connections_available_ = false;
            schedules_available_ = false;
            file_browse_available_ = false;
            file_recover_browse_available_ = false;
            file_restore_available_ = false;
            backup_start_available_ = false;
            restore_preflight_available_ = false;
            restore_start_available_ = false;
            ntfs_shrink_available_ = false;
            pe_restore_->set_available(false);
            restore_command_busy_ = false;
            mount_list_available_ = false;
            mount_start_available_ = false;
            mount_unmount_available_ = false;
            mount_command_busy_ = false;
            job_cancel_available_ = false;
            handshake_complete_ = false;
            reset_repository_command();
            stop_repository_refresh();
            reset_backup_command();
            reset_mount_command();
        } else {
            // Pre-ready connect churn: clear handshake only (avoid model/signal storms).
            handshake_complete_ = false;
            service_version_.clear();
            api_version_ = 0;
            capabilities_.clear();
        }
        if (!first_ready_seen_ && state == State::kDisconnected) {
            if (splash_connect_timer_ == nullptr || !splash_connect_timer_->isActive()) {
                splash_error_ = true;
                // Hold splash on error until Retry (matches old AegraImage; stops Loading/Error
                // flicker).
                transport_->set_auto_reconnect_enabled(false);
            }
        } else if (first_ready_seen_ && state == State::kDisconnected) {
            transport_->ensure_reconnect_scheduled();
        }
    } else {
        transport_->set_auto_reconnect_enabled(true);
    }

    emit stateChanged();
    update_splash_for_state();
    emit loadingChanged();
}

void ServiceClient::update_format_locale() {
    if (locale_controller_ != nullptr) {
        format_.set_locale(locale_controller_->locale());
    }
}

void ServiceClient::update_job_polling() {
    if (state_ == State::kReady && job_list_available_ &&
        (jobs_.has_active_jobs() || pending_terminal_job_sync_ ||
         QDateTime::currentMSecsSinceEpoch() < job_chain_watch_deadline_ms_)) {
        if (!job_poll_timer_->isActive()) {
            job_poll_timer_->start();
        }
    } else {
        job_poll_timer_->stop();
    }
}

void ServiceClient::apply_active_job_snapshot(QVector<JobRow> rows) {
    const auto vanished = jobs_.replace_active_jobs(std::move(rows));
    for (const auto& job_id : vanished) {
        awaiting_terminal_job_ids_.insert(job_id);
    }
    pending_terminal_job_sync_ = !awaiting_terminal_job_ids_.isEmpty();
    if (!vanished.isEmpty()) {
        // A job just left the active set; the post-backup chain may submit the
        // next one (verify, then boot check) shortly.
        job_chain_watch_deadline_ms_ =
            QDateTime::currentMSecsSinceEpoch() + kPostBackupWatchMilliseconds;
    }
}

void ServiceClient::apply_terminal_job_seed(QVector<JobRow> rows) {
    if (!jobs_baseline_seeded_) {
        seed_terminal_toast_baseline(rows);
        jobs_baseline_seeded_ = true;
    } else {
        publish_terminal_toasts(rows);
    }
    jobs_.merge_terminal_jobs(std::move(rows));
    resolve_awaiting_terminal_jobs();
}

void ServiceClient::resolve_awaiting_terminal_jobs() {
    QSet<QString> still;
    still.reserve(awaiting_terminal_job_ids_.size());
    for (const auto& job_id : awaiting_terminal_job_ids_) {
        const auto job = jobs_.find_job(job_id);
        if (!job) {
            still.insert(job_id);
            continue;
        }
        if (job->state == 1 || job->state == 2 || job->state == 3) {
            still.insert(job_id);
        }
    }
    awaiting_terminal_job_ids_ = std::move(still);
    pending_terminal_job_sync_ = !awaiting_terminal_job_ids_.isEmpty();
}

void ServiceClient::observe_accepted_restore_job(const QString& job_id) {
    if (!job_id.isEmpty()) {
        JobRow optimistic;
        optimistic.job_id = job_id;
        optimistic.operation = 2;
        optimistic.state = 2;
        optimistic.created_utc_ms = QDateTime::currentMSecsSinceEpoch();
        enrich_job_row(optimistic);
        jobs_.upsert_job(std::move(optimistic));
        emit jobsChanged();
    }
    if (job_list_available_ && !jobs_loading_ &&
        (job_request_id_.isEmpty() || !coordinator_->has_pending_request(job_request_id_))) {
        start_job_query();
        return;
    }
    update_job_polling();
}

void ServiceClient::seed_terminal_toast_baseline(const QVector<JobRow>& rows) {
    for (const auto& row : rows) {
        if (row.state != 4 && row.state != 5 && row.state != 6 && row.state != 7) {
            continue;
        }
        toasted_job_keys_.insert(job_toast_key(row));
    }
}

void ServiceClient::publish_terminal_toasts(const QVector<JobRow>& rows) {
    QString latest_toast;
    bool latest_is_error = false;
    bool found_new_event = false;
    for (const auto& row : rows) {
        if (row.state != 4 && row.state != 5 && row.state != 6 && row.state != 7) {
            continue;
        }
        const auto key = job_toast_key(row);
        if (toasted_job_keys_.contains(key)) {
            continue;
        }
        toasted_job_keys_.insert(key);
        found_new_event = true;
        latest_toast = terminal_job_toast_text(row);
        latest_is_error = row.state != 4;
    }
    if (!latest_toast.isEmpty()) {
        show_toast(latest_toast, latest_is_error);
    }
    if (found_new_event && !has_unread_events_) {
        has_unread_events_ = true;
        emit unreadEventsChanged();
    }
}

void ServiceClient::show_toast(const QString& text, const bool is_error) {
    ++toast_generation_;
    toast_text_ = text;
    toast_is_error_ = is_error;
    toast_visible_ = true;
    emit toastChanged();
    // Single restartable timer: consecutive toasts reset the full 4s visibility window.
    toast_timer_->start();
}

void ServiceClient::showToast(const QString& text, const bool isError) {
    if (text.trimmed().isEmpty()) {
        return;
    }
    show_toast(text, isError);
}

QString ServiceClient::firstSelectableSourceId() const {
    for (int row = 0; row < sources_.rowCount(); ++row) {
        const auto index = sources_.index(row, 0);
        if (sources_.data(index, SourceInventoryModel::IsSelectableRole).toBool()) {
            return sources_.data(index, SourceInventoryModel::SourceIdRole).toString();
        }
    }
    return {};
}

void ServiceClient::on_splash_connect_timeout() {
    if (first_ready_seen_ || state_ == State::kReady) {
        return;
    }
    transport_->set_auto_reconnect_enabled(false);
    splash_error_ = true;
    const auto err = pending_splash_error_code_.isEmpty()
                         ? QStringLiteral("service.connect_failed")
                         : pending_splash_error_code_;
    set_state(State::kDisconnected, err);
    update_splash_for_state();
}

void ServiceClient::update_splash_for_state() { emit splashChanged(); }

} // namespace aegra::desktop
