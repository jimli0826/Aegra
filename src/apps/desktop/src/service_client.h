#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstdint>
#include <optional>

class QJsonObject;
class QLocalSocket;
class QTimer;

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
    Q_PROPERTY(QVariantList recoveryPoints READ recoveryPoints NOTIFY repositoryChanged)

  public:
    explicit ServiceClient(QObject* parent = nullptr);

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
    [[nodiscard]] QVariantList recoveryPoints() const;

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

    enum class PendingRequest : std::uint8_t {
        kNone,
        kServiceInfo,
        kRecoveryPoints,
    };

    void on_connected();
    void on_disconnected();
    void on_ready_read();
    void on_socket_error();
    void send_service_info_request();
    void start_repository_query();
    void send_recovery_point_request(const std::optional<QString>& token);
    void consume_frames();
    [[nodiscard]] bool apply_response(const QByteArray& frame);
    [[nodiscard]] bool apply_service_info(const QJsonObject& root);
    [[nodiscard]] bool apply_recovery_point_page(const QJsonObject& root);
    void finish_repository_failure();
    void reset_repository();
    void fail_protocol();
    void schedule_reconnect();
    void set_state(State state, QString error = {});

    QLocalSocket* socket_{nullptr};
    QTimer* reconnect_timer_{nullptr};
    QByteArray input_;
    quint32 expected_frame_bytes_{0};
    QString request_id_;
    QString service_version_;
    QStringList capabilities_;
    QString error_text_;
    QString repository_uuid_;
    QString repository_error_text_;
    QVariantList recovery_points_;
    QVariantList pending_recovery_points_;
    std::optional<QString> requested_token_;
    QString last_file_uuid_;
    quint32 api_version_{0};
    State state_{State::kDisconnected};
    PendingRequest pending_request_{PendingRequest::kNone};
    bool repository_configured_{false};
    bool repository_loading_{false};
};
