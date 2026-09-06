#include "client/service_client.h"

#include "client/service_protocol.h"
#include "client/service_request_coordinator.h"
#include "locale/message_code_map.h"

#include <QJsonObject>
#include <QUuid>

#include <algorithm>

namespace aegra::desktop {

bool ServiceClient::serviceSettingsAvailable() const noexcept {
    return service_settings_available_;
}
bool ServiceClient::serviceSettingsLoading() const noexcept { return service_settings_loading_; }
bool ServiceClient::serviceSettingsBusy() const noexcept { return service_settings_busy_; }
int ServiceClient::jobRetentionMonths() const noexcept { return job_retention_months_; }
int ServiceClient::defaultBootCheckHypervisor() const noexcept {
    return default_boot_check_hypervisor_;
}
int ServiceClient::bootCheckCpuCount() const noexcept { return boot_check_cpu_count_; }
int ServiceClient::bootCheckMemoryMib() const noexcept { return boot_check_memory_mib_; }
int ServiceClient::bootCheckConcurrency() const noexcept { return boot_check_concurrency_; }
int ServiceClient::bootCheckEffectiveConcurrency() const noexcept {
    return boot_check_effective_concurrency_;
}
int ServiceClient::verifyScope() const noexcept { return verify_scope_; }
int ServiceClient::verifyConcurrency() const noexcept { return verify_concurrency_; }
int ServiceClient::hostLogicalCpuCount() const noexcept { return host_logical_cpu_count_; }
qint64 ServiceClient::hostPhysicalMemoryMib() const noexcept { return host_physical_memory_mib_; }
qint64 ServiceClient::bootCheckMemoryBudgetMib() const noexcept {
    return boot_check_memory_budget_mib_;
}

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
        service_settings_loading_ ||
        (months != kJobRetentionMonths1 && months != kJobRetentionMonths3 &&
         months != kJobRetentionMonths6)) {
        return false;
    }
    if (months == job_retention_months_) {
        return true;
    }
    pending_job_retention_months_ = months;
    pending_default_boot_check_hypervisor_ = default_boot_check_hypervisor_;
    pending_boot_check_cpu_count_ = boot_check_cpu_count_;
    pending_boot_check_memory_mib_ = boot_check_memory_mib_;
    pending_boot_check_concurrency_ = boot_check_concurrency_;
    pending_verify_scope_ = verify_scope_;
    pending_verify_concurrency_ = verify_concurrency_;
    return begin_service_settings_update();
}

bool ServiceClient::setBootCheckSettings(const int default_hypervisor, const int cpu_count,
                                         const int memory_mib, const int concurrency) {
    if (state_ != State::kReady || !service_settings_available_ || service_settings_busy_ ||
        service_settings_loading_ || (default_hypervisor != 1 && default_hypervisor != 2) ||
        cpu_count < 1 || cpu_count > host_logical_cpu_count_ ||
        memory_mib < 2048 || memory_mib > host_physical_memory_mib_ || concurrency < 1 ||
        concurrency > 32) {
        return false;
    }
    if (default_hypervisor == default_boot_check_hypervisor_ &&
        cpu_count == boot_check_cpu_count_ && memory_mib == boot_check_memory_mib_ &&
        concurrency == boot_check_concurrency_) {
        return true;
    }
    pending_job_retention_months_ = job_retention_months_;
    pending_default_boot_check_hypervisor_ = default_hypervisor;
    pending_boot_check_cpu_count_ = cpu_count;
    pending_boot_check_memory_mib_ = memory_mib;
    pending_boot_check_concurrency_ = concurrency;
    pending_verify_scope_ = verify_scope_;
    pending_verify_concurrency_ = verify_concurrency_;
    return begin_service_settings_update();
}

bool ServiceClient::setVerifySettings(const int scope, const int concurrency) {
    if (state_ != State::kReady || !service_settings_available_ || service_settings_busy_ ||
        service_settings_loading_ || (scope != 1 && scope != 2) || concurrency < 1 ||
        concurrency > 32) {
        return false;
    }
    if (scope == verify_scope_ && concurrency == verify_concurrency_) {
        return true;
    }
    pending_job_retention_months_ = job_retention_months_;
    pending_default_boot_check_hypervisor_ = default_boot_check_hypervisor_;
    pending_boot_check_cpu_count_ = boot_check_cpu_count_;
    pending_boot_check_memory_mib_ = boot_check_memory_mib_;
    pending_boot_check_concurrency_ = boot_check_concurrency_;
    pending_verify_scope_ = scope;
    pending_verify_concurrency_ = concurrency;
    return begin_service_settings_update();
}

bool ServiceClient::begin_service_settings_update() {
    service_settings_busy_ = true;
    service_settings_error_code_.clear();
    service_settings_update_idempotency_key_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit serviceSettingsChanged();
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    service_settings_update_request_id_ = request_id;
    const auto body = encode_update_service_settings_request(
        request_id, service_settings_update_idempotency_key_, pending_job_retention_months_,
        pending_default_boot_check_hypervisor_, pending_boot_check_cpu_count_,
        pending_boot_check_memory_mib_, pending_boot_check_concurrency_, pending_verify_scope_,
        pending_verify_concurrency_);
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
        finish_service_settings_failure(QStringLiteral("service.settings_failed"));
        return RequestDisposition::kProtocolError;
    }
    job_retention_months_ = settings.job_retention_months;
    default_boot_check_hypervisor_ = settings.default_boot_check_hypervisor;
    boot_check_cpu_count_ = settings.boot_check_cpu_count;
    boot_check_memory_mib_ = settings.boot_check_memory_mib;
    boot_check_concurrency_ = settings.boot_check_concurrency;
    boot_check_effective_concurrency_ = settings.boot_check_effective_concurrency;
    verify_scope_ = settings.verify_scope;
    verify_concurrency_ = settings.verify_concurrency;
    host_logical_cpu_count_ = settings.host_logical_cpu_count;
    host_physical_memory_mib_ = settings.host_physical_memory_mib;
    boot_check_memory_budget_mib_ = settings.boot_check_memory_budget_mib;
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
        finish_service_settings_failure(QStringLiteral("service.settings_update_failed"));
        return RequestDisposition::kProtocolError;
    }
    job_retention_months_ = pending_job_retention_months_;
    default_boot_check_hypervisor_ = pending_default_boot_check_hypervisor_;
    boot_check_cpu_count_ = pending_boot_check_cpu_count_;
    boot_check_memory_mib_ = pending_boot_check_memory_mib_;
    boot_check_concurrency_ = pending_boot_check_concurrency_;
    verify_scope_ = pending_verify_scope_;
    verify_concurrency_ = pending_verify_concurrency_;
    boot_check_effective_concurrency_ = static_cast<int>((std::min)(
        static_cast<qint64>(boot_check_concurrency_),
        boot_check_memory_mib_ == 0 ? 0 : boot_check_memory_budget_mib_ / boot_check_memory_mib_));
    service_settings_busy_ = false;
    service_settings_error_code_.clear();
    service_settings_update_request_id_.clear();
    service_settings_update_idempotency_key_.clear();
    emit serviceSettingsChanged();
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
    default_boot_check_hypervisor_ = pending_default_boot_check_hypervisor_ = 1;
    boot_check_cpu_count_ = pending_boot_check_cpu_count_ = 1;
    boot_check_memory_mib_ = pending_boot_check_memory_mib_ = 4096;
    boot_check_concurrency_ = pending_boot_check_concurrency_ = 2;
    boot_check_effective_concurrency_ = 2;
    verify_scope_ = pending_verify_scope_ = 1;
    verify_concurrency_ = pending_verify_concurrency_ = 2;
    host_logical_cpu_count_ = 1;
    host_physical_memory_mib_ = 2048;
    boot_check_memory_budget_mib_ = 0;
    service_settings_error_code_.clear();
    service_settings_request_id_.clear();
    service_settings_update_request_id_.clear();
    service_settings_update_idempotency_key_.clear();
    emit serviceSettingsChanged();
}

} // namespace aegra::desktop
