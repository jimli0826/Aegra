#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

namespace aegra::desktop {

inline constexpr quint32 kServiceSchemaVersion = 2;
inline constexpr quint32 kServiceApiVersion = 2;
inline constexpr quint32 kRecoveryPointPageSize = 100;

struct ServiceInfo final {
    QString version;
    QStringList capabilities;
};

struct RecoveryPointPage final {
    bool configured{false};
    QString repository_uuid;
    QVariantList items;
    std::optional<QString> continuation_token;
};

[[nodiscard]] QByteArray encode_service_info_request(const QString& request_id);
[[nodiscard]] QByteArray
encode_recovery_point_request(const QString& request_id,
                              const std::optional<QString>& continuation_token);

[[nodiscard]] bool parse_response_root(const QByteArray& body, const QString& request_id,
                                       QJsonObject& root);
[[nodiscard]] bool parse_service_info_response(const QJsonObject& root, ServiceInfo& result);
[[nodiscard]] bool parse_recovery_point_response(const QJsonObject& root,
                                                 RecoveryPointPage& result);
[[nodiscard]] bool is_repository_failure_response(const QJsonObject& root);

} // namespace aegra::desktop
