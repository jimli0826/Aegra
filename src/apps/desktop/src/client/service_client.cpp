#include "client/service_client.h"

#include "client/ipc_frame_transport.h"
#include "client/service_protocol.h"
#include "client/service_request_coordinator.h"
#include "locale/locale_controller.h"
#include "locale/message_code_map.h"

#include <QJsonObject>
#include <QTimer>
#include <QUuid>

#include <utility>

namespace aegra::desktop {
namespace {

constexpr auto kServicePipeName = "aegra-service-control";
constexpr qsizetype kMaximumRecoveryPoints = 10'000;

} // namespace

ServiceClient::ServiceClient(QObject* parent)
    : QObject(parent), recovery_points_(this),
      transport_(std::make_unique<IpcFrameTransport>(QLatin1String(kServicePipeName))),
      coordinator_(std::make_unique<ServiceRequestCoordinator>(*transport_)) {
    recovery_points_.set_locale_format(&format_);
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

QString ServiceClient::errorText() const {
    if (error_code_.isEmpty()) {
        return {};
    }
    return localize_message_code(error_code_);
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
    if (repository_error_code_.isEmpty()) {
        return {};
    }
    return localize_message_code(repository_error_code_);
}

RecoveryPointModel* ServiceClient::recoveryPoints() noexcept { return &recovery_points_; }

int ServiceClient::recoveryPointCount() const { return recovery_points_.rowCount(); }

void ServiceClient::reconnect() {
    set_state(State::kConnecting);
    transport_->connect_to_service();
}

void ServiceClient::refreshRepository() {
    if (state_ != State::kReady || coordinator_->has_pending_request() || repository_loading_) {
        return;
    }
    start_repository_query();
}

void ServiceClient::on_transport_connected() {
    handshake_complete_ = false;
    send_service_info_request();
}

void ServiceClient::on_transport_disconnected() {
    if (state_ == State::kDisconnected) {
        return;
    }
    set_state(State::kDisconnected, QStringLiteral("service.disconnected"));
}

void ServiceClient::on_transport_error(const QString& message_code) {
    set_state(State::kDisconnected, message_code);
}

void ServiceClient::on_request_failed(const QString& message_code) {
    if (message_code == QLatin1String("service.protocol_invalid") ||
        message_code == QLatin1String("service.request_timeout") ||
        message_code == QLatin1String("service.send_failed")) {
        set_state(State::kDisconnected, message_code);
        transport_->disconnect_from_service();
        QTimer::singleShot(0, transport_.get(), &IpcFrameTransport::connect_to_service);
        return;
    }
    if (!handshake_complete_) {
        set_state(State::kDisconnected, message_code);
        return;
    }
    if (repository_loading_) {
        finish_repository_failure(QStringLiteral("repository.query_failed"));
    }
}

void ServiceClient::on_locale_changed() {
    update_format_locale();
    recovery_points_.retranslate();
    emit stateChanged();
    emit repositoryChanged();
}

void ServiceClient::send_service_info_request() {
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto body = encode_service_info_request(request_id);
    const auto started = coordinator_->begin_request(
        request_id, body,
        [this](const QByteArray& frame_body) { return handle_service_info_frame(frame_body); });
    if (!started) {
        set_state(State::kDisconnected, QStringLiteral("service.send_failed"));
    }
}

void ServiceClient::start_repository_query() {
    reset_repository();
    repository_loading_ = true;
    emit repositoryChanged();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    requested_token_.reset();
    const auto body = encode_recovery_point_request(request_id, std::nullopt);
    const auto started = coordinator_->begin_request(
        request_id, body,
        [this](const QByteArray& frame_body) { return handle_recovery_point_frame(frame_body); });
    if (!started) {
        finish_repository_failure(QStringLiteral("repository.query_failed"));
    }
}

RequestDisposition ServiceClient::handle_service_info_frame(const QByteArray& body) {
    QJsonObject root;
    if (!parse_response_root(body, coordinator_->pending_request_id(), root)) {
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
    handshake_complete_ = true;
    // Defer the follow-up query until the coordinator finishes the handshake request.
    QTimer::singleShot(0, this, [this]() {
        if (!handshake_complete_) {
            return;
        }
        set_state(State::kReady);
        start_repository_query();
    });
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_recovery_point_frame(const QByteArray& body) {
    QJsonObject root;
    if (!parse_response_root(body, coordinator_->pending_request_id(), root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_repository_failure_response(root)) {
        finish_repository_failure(QStringLiteral("repository.query_failed"));
        return RequestDisposition::kFinished;
    }

    RecoveryPointPage page;
    if (!parse_recovery_point_response(root, page)) {
        return RequestDisposition::kProtocolError;
    }
    if (!page.configured) {
        if (requested_token_ || !pending_recovery_points_.isEmpty()) {
            return RequestDisposition::kProtocolError;
        }
        repository_loading_ = false;
        emit repositoryChanged();
        return RequestDisposition::kFinished;
    }
    if ((!repository_uuid_.isEmpty() && page.repository_uuid != repository_uuid_) ||
        (page.continuation_token && page.continuation_token == requested_token_) ||
        pending_recovery_points_.size() + page.items.size() > kMaximumRecoveryPoints) {
        return RequestDisposition::kProtocolError;
    }
    repository_uuid_ = page.repository_uuid;
    for (auto& item : page.items) {
        const auto file_uuid = item.toMap().value(QStringLiteral("fileUuid")).toString();
        if (!last_file_uuid_.isEmpty() && file_uuid <= last_file_uuid_) {
            return RequestDisposition::kProtocolError;
        }
        last_file_uuid_ = file_uuid;
        pending_recovery_points_.push_back(std::move(item));
    }
    if (page.continuation_token) {
        requested_token_ = page.continuation_token;
        const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto next_body = encode_recovery_point_request(request_id, requested_token_);
        if (!coordinator_->continue_request(request_id, next_body)) {
            return RequestDisposition::kProtocolError;
        }
        return RequestDisposition::kContinue;
    }
    recovery_points_.set_rows(recovery_points_from_variant_list(pending_recovery_points_));
    pending_recovery_points_.clear();
    repository_configured_ = true;
    repository_loading_ = false;
    requested_token_.reset();
    emit repositoryChanged();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_repository_failure(const QString& message_code) {
    repository_uuid_.clear();
    pending_recovery_points_.clear();
    requested_token_.reset();
    last_file_uuid_.clear();
    repository_loading_ = false;
    repository_configured_ = false;
    recovery_points_.clear();
    repository_error_code_ = message_code;
    emit repositoryChanged();
}

void ServiceClient::reset_repository() {
    repository_uuid_.clear();
    repository_error_code_.clear();
    recovery_points_.clear();
    pending_recovery_points_.clear();
    requested_token_.reset();
    last_file_uuid_.clear();
    repository_configured_ = false;
    repository_loading_ = false;
    emit repositoryChanged();
}

void ServiceClient::set_state(const State state, QString error_code) {
    state_ = state;
    error_code_ = std::move(error_code);
    if (state != State::kReady) {
        service_version_.clear();
        api_version_ = 0;
        capabilities_.clear();
        handshake_complete_ = false;
        reset_repository();
    }
    emit stateChanged();
}

void ServiceClient::update_format_locale() {
    if (locale_controller_ != nullptr) {
        format_.set_locale(locale_controller_->locale());
    }
}

} // namespace aegra::desktop
