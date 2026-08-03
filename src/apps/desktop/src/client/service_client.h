#pragma once

#include "client/models/recovery_point_model.h"
#include "client/service_request_coordinator.h"
#include "locale/locale_format.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <optional>

namespace aegra::desktop {

class IpcFrameTransport;
class LocaleController;

// Desktop composition facade over transport, protocol codec, request coordinator, and domain
// models. QML observes owned properties only and never parses JSON or Service message codes.
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

    Q_INVOKABLE void reconnect();
    Q_INVOKABLE void refreshRepository();

  signals:
    void stateChanged();
    void repositoryChanged();

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
    void send_service_info_request();
    void start_repository_query();
    [[nodiscard]] RequestDisposition handle_service_info_frame(const QByteArray& body);
    [[nodiscard]] RequestDisposition handle_recovery_point_frame(const QByteArray& body);
    void finish_repository_failure(const QString& message_code);
    void reset_repository();
    void set_state(State state, QString error_code = {});
    void update_format_locale();

    LocaleController* locale_controller_{nullptr};
    LocaleFormat format_;
    RecoveryPointModel recovery_points_;
    std::unique_ptr<IpcFrameTransport> transport_;
    std::unique_ptr<ServiceRequestCoordinator> coordinator_;
    QString service_version_;
    QStringList capabilities_;
    QString error_code_;
    QString repository_uuid_;
    QString repository_error_code_;
    QVariantList pending_recovery_points_;
    std::optional<QString> requested_token_;
    QString last_file_uuid_;
    quint32 api_version_{0};
    State state_{State::kDisconnected};
    bool repository_configured_{false};
    bool repository_loading_{false};
    bool handshake_complete_{false};
};

} // namespace aegra::desktop
