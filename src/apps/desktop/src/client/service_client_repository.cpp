#include "client/service_client.h"

#include "client/service_protocol.h"
#include "locale/message_code_map.h"

#include <QJsonObject>
#include <QUuid>

namespace aegra::desktop {
namespace {

constexpr qsizetype kMaximumRecoveryPoints = 10'000;

[[nodiscard]] QString new_request_id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

[[nodiscard]] QString new_idempotency_key() {
    return QStringLiteral("desktop-") + new_request_id();
}

} // namespace

void ServiceClient::start_repository_query() {
    reset_repository();
    repository_loading_ = true;
    emit repositoryChanged();
    emit loadingChanged();

    const auto request_id = new_request_id();
    repository_request_id_ = request_id;
    requested_token_.reset();
    const std::optional<QString> connection_id =
        selected_repository_connection_id_.isEmpty()
            ? std::nullopt
            : std::optional{selected_repository_connection_id_};
    const auto body = encode_recovery_point_request(request_id, std::nullopt, connection_id);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_recovery_point_frame(frame_body);
        });
    if (!started) {
        finish_repository_failure(QStringLiteral("repository.query_failed"));
    }
}

RequestDisposition ServiceClient::handle_recovery_point_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
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
    const std::optional<QString> expected_connection =
        selected_repository_connection_id_.isEmpty()
            ? std::nullopt
            : std::optional{selected_repository_connection_id_};
    if (page.repository_connection_id != expected_connection) {
        return RequestDisposition::kProtocolError;
    }
    if (!page.configured) {
        if (requested_token_ || !pending_recovery_points_.isEmpty()) {
            return RequestDisposition::kProtocolError;
        }
        repository_loading_ = false;
        repository_request_id_.clear();
        emit repositoryChanged();
        emit loadingChanged();
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
        const auto next_id = new_request_id();
        const auto next_body =
            encode_recovery_point_request(next_id, requested_token_, expected_connection);
        if (!coordinator_->continue_request(request_id, next_id, next_body)) {
            return RequestDisposition::kProtocolError;
        }
        repository_request_id_ = next_id;
        return RequestDisposition::kContinue;
    }
    recovery_points_.set_rows(recovery_points_from_variant_list(pending_recovery_points_));
    pending_recovery_points_.clear();
    repository_configured_ = true;
    repository_loading_ = false;
    repository_request_id_.clear();
    requested_token_.reset();
    emit repositoryChanged();
    emit loadingChanged();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_repository_failure(const QString& message_code) {
    repository_uuid_.clear();
    pending_recovery_points_.clear();
    requested_token_.reset();
    last_file_uuid_.clear();
    repository_loading_ = false;
    repository_configured_ = false;
    repository_request_id_.clear();
    recovery_points_.clear();
    repository_error_code_ = message_code;
    emit repositoryChanged();
    emit loadingChanged();
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
    repository_request_id_.clear();
    emit repositoryChanged();
    emit loadingChanged();
}

QString ServiceClient::selectedRepositoryConnectionId() const {
    return selected_repository_connection_id_;
}

bool ServiceClient::repositoryCommandBusy() const noexcept { return repository_command_busy_; }

QString ServiceClient::repositoryCommandErrorText() const {
    return repository_command_error_code_.isEmpty()
               ? QString{}
               : localize_message_code(repository_command_error_code_);
}

void ServiceClient::selectRepositoryConnection(const QString& connection_id) {
    if (connection_id.isEmpty() || !connections_.find(connection_id)) {
        return;
    }
    const bool changed = selected_repository_connection_id_ != connection_id;
    selected_repository_connection_id_ = connection_id;
    if (changed) {
        emit repositoryChanged();
    }
    refreshRepository();
}

void ServiceClient::addRepositoryConnection(const QString& display_name, const QString& locator) {
    start_repository_input_command(kAddRepositoryConnectionRequestKind, display_name, locator);
}

void ServiceClient::importRepositoryConnection(const QString& display_name,
                                               const QString& locator) {
    start_repository_input_command(kImportRepositoryConnectionRequestKind, display_name, locator);
}

void ServiceClient::testRepositoryConnection(const QString& connection_id) {
    start_repository_resource_command(kTestRepositoryConnectionRequestKind, connection_id);
}

void ServiceClient::setDefaultRepositoryConnection(const QString& connection_id) {
    start_repository_resource_command(kSetDefaultRepositoryRequestKind, connection_id);
}

void ServiceClient::removeRepositoryConnection(const QString& connection_id) {
    start_repository_resource_command(kRemoveRepositoryConnectionRequestKind, connection_id);
}

void ServiceClient::start_repository_input_command(const int request_kind,
                                                   const QString& display_name,
                                                   const QString& locator) {
    if (!connected() || repository_command_busy_ || display_name.trimmed().isEmpty() ||
        locator.trimmed().isEmpty()) {
        return;
    }
    repository_command_busy_ = true;
    repository_command_error_code_.clear();
    repository_command_kind_ = request_kind;
    repository_command_request_id_ = new_request_id();
    repository_command_idempotency_key_ = new_idempotency_key();
    emit repositoryCommandChanged();
    const auto body = encode_repository_connection_input_request(
        repository_command_request_id_, repository_command_idempotency_key_, request_kind,
        display_name.trimmed(), locator.trimmed());
    const auto started = coordinator_->begin_request(
        repository_command_request_id_, body, [this](const QByteArray& frame_body) {
            return handle_repository_command_frame(frame_body);
        });
    if (!started) {
        finish_repository_command_failure(QStringLiteral("service.send_failed"));
    }
}

void ServiceClient::start_repository_resource_command(const int request_kind,
                                                      const QString& connection_id) {
    if (!connected() || repository_command_busy_ || !connections_.find(connection_id)) {
        return;
    }
    repository_command_busy_ = true;
    repository_command_error_code_.clear();
    repository_command_kind_ = request_kind;
    repository_command_request_id_ = new_request_id();
    repository_command_idempotency_key_ = new_idempotency_key();
    emit repositoryCommandChanged();
    const auto body = encode_repository_connection_resource_request(
        repository_command_request_id_, repository_command_idempotency_key_, request_kind,
        connection_id);
    const auto started = coordinator_->begin_request(
        repository_command_request_id_, body, [this](const QByteArray& frame_body) {
            return handle_repository_command_frame(frame_body);
        });
    if (!started) {
        finish_repository_command_failure(QStringLiteral("service.send_failed"));
    }
}

RequestDisposition ServiceClient::handle_repository_command_frame(const QByteArray& body) {
    QJsonObject root;
    if (!parse_response_root(body, repository_command_request_id_, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, repository_command_kind_)) {
        const auto message_code = root.value(QStringLiteral("message_code")).toString();
        finish_repository_command_failure(
            message_code.isEmpty() ? QStringLiteral("service.request_failed") : message_code);
        refreshConnections();
        return RequestDisposition::kFinished;
    }
    CommandAck acknowledgement;
    if (!parse_command_ack_response(root, repository_command_kind_, acknowledgement)) {
        return RequestDisposition::kProtocolError;
    }
    if (repository_command_kind_ == kRemoveRepositoryConnectionRequestKind &&
        acknowledgement.resource_id == selected_repository_connection_id_) {
        selected_repository_connection_id_.clear();
        reset_repository();
    } else if (acknowledgement.has_resource_id) {
        selected_repository_connection_id_ = acknowledgement.resource_id;
    }
    reset_repository_command();
    refreshConnections();
    emit repositoryChanged();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_repository_command_failure(const QString& message_code) {
    repository_command_busy_ = false;
    repository_command_request_id_.clear();
    repository_command_idempotency_key_.clear();
    repository_command_kind_ = 0;
    repository_command_error_code_ = message_code;
    emit repositoryCommandChanged();
}

void ServiceClient::reset_repository_command() {
    repository_command_busy_ = false;
    repository_command_request_id_.clear();
    repository_command_idempotency_key_.clear();
    repository_command_kind_ = 0;
    repository_command_error_code_.clear();
    emit repositoryCommandChanged();
}

} // namespace aegra::desktop
