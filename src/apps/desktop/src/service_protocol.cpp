#include "service_protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <algorithm>
#include <initializer_list>
#include <limits>

namespace aegra::desktop {
namespace {

constexpr qsizetype kMaximumVersionCharacters = 64;
constexpr qsizetype kMaximumCapabilities = 64;
constexpr qsizetype kMaximumCapabilityCharacters = 64;
constexpr qsizetype kMaximumContinuationTokenCharacters = 1'024;

[[nodiscard]] bool has_exact_keys(const QJsonObject& object,
                                  const std::initializer_list<const char*> keys) {
    if (object.size() != static_cast<int>(keys.size())) {
        return false;
    }
    return std::all_of(keys.begin(), keys.end(),
                       [&object](const char* key) { return object.contains(QLatin1String(key)); });
}

[[nodiscard]] bool integer_in_range(const QJsonValue& value, const qint64 minimum,
                                    const qint64 maximum, qint64& result) {
    if (!value.isDouble()) {
        return false;
    }
    const auto integer = value.toInteger((std::numeric_limits<qint64>::min)());
    if (integer < minimum || integer > maximum ||
        value.toDouble() != static_cast<double>(integer)) {
        return false;
    }
    result = integer;
    return true;
}

[[nodiscard]] bool stable_code(const QString& value, const qsizetype maximum_characters) {
    if (value.isEmpty() || value.size() > maximum_characters) {
        return false;
    }
    return std::all_of(value.cbegin(), value.cend(), [](const QChar character) {
        const auto code = character.unicode();
        return (code >= 'a' && code <= 'z') || (code >= '0' && code <= '9') || code == '.' ||
               code == '_' || code == '-';
    });
}

[[nodiscard]] bool canonical_uuid(const QString& value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
        value[23] != '-') {
        return false;
    }
    for (qsizetype index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        const auto code = value[index].unicode();
        if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f'))) {
            return false;
        }
    }
    return value[14] >= '1' && value[14] <= '8' &&
           (value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
}

[[nodiscard]] bool optional_uuid(const QJsonValue& value, QString& result, bool& present) {
    if (value.isNull()) {
        result.clear();
        present = false;
        return true;
    }
    if (!value.isString() || !canonical_uuid(value.toString())) {
        return false;
    }
    result = value.toString();
    present = true;
    return true;
}

[[nodiscard]] bool parse_recovery_point(const QJsonValue& value, QVariantMap& result) {
    if (!value.isObject()) {
        return false;
    }
    const auto object = value.toObject();
    if (!has_exact_keys(object, {"file_uuid", "backup_set_uuid", "parent_uuid", "backup_type",
                                 "chain_state", "created_utc_ms", "logical_size_bytes",
                                 "stored_size_bytes", "source_count", "has_sidecar"})) {
        return false;
    }
    const auto file_uuid = object.value(QStringLiteral("file_uuid")).toString();
    const auto backup_set_uuid = object.value(QStringLiteral("backup_set_uuid")).toString();
    QString parent_uuid;
    bool has_parent = false;
    qint64 backup_type = 0;
    qint64 chain_state = 0;
    qint64 created_utc_ms = 0;
    qint64 logical_size_bytes = 0;
    qint64 stored_size_bytes = 0;
    qint64 source_count = 0;
    if (!canonical_uuid(file_uuid) || !canonical_uuid(backup_set_uuid) ||
        !optional_uuid(object.value(QStringLiteral("parent_uuid")), parent_uuid, has_parent) ||
        !integer_in_range(object.value(QStringLiteral("backup_type")), 1, 3, backup_type) ||
        !integer_in_range(object.value(QStringLiteral("chain_state")), 1, 2, chain_state) ||
        !integer_in_range(object.value(QStringLiteral("created_utc_ms")), 0,
                          (std::numeric_limits<qint64>::max)(), created_utc_ms) ||
        !integer_in_range(object.value(QStringLiteral("logical_size_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), logical_size_bytes) ||
        !integer_in_range(object.value(QStringLiteral("stored_size_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), stored_size_bytes) ||
        !integer_in_range(object.value(QStringLiteral("source_count")), 0,
                          (std::numeric_limits<quint32>::max)(), source_count) ||
        !object.value(QStringLiteral("has_sidecar")).isBool()) {
        return false;
    }
    if ((backup_type == 1) != !has_parent || (has_parent && parent_uuid == file_uuid)) {
        return false;
    }
    if (backup_type == 1 && chain_state != 1) {
        return false;
    }
    result = {{QStringLiteral("fileUuid"), file_uuid},
              {QStringLiteral("backupSetUuid"), backup_set_uuid},
              {QStringLiteral("parentUuid"), parent_uuid},
              {QStringLiteral("backupType"), backup_type},
              {QStringLiteral("chainState"), chain_state},
              {QStringLiteral("createdUtcMs"), created_utc_ms},
              {QStringLiteral("logicalSizeBytes"), logical_size_bytes},
              {QStringLiteral("storedSizeBytes"), stored_size_bytes},
              {QStringLiteral("sourceCount"), source_count},
              {QStringLiteral("hasSidecar"), object.value(QStringLiteral("has_sidecar")).toBool()}};
    return true;
}

[[nodiscard]] bool parse_items(const QJsonValue& value, QVariantList& result) {
    if (!value.isArray()) {
        return false;
    }
    const auto items = value.toArray();
    if (items.size() > static_cast<qsizetype>(kRecoveryPointPageSize)) {
        return false;
    }
    QString previous_uuid;
    for (const auto& item : items) {
        QVariantMap parsed;
        if (!parse_recovery_point(item, parsed)) {
            return false;
        }
        const auto file_uuid = parsed.value(QStringLiteral("fileUuid")).toString();
        if (!previous_uuid.isEmpty() && file_uuid <= previous_uuid) {
            return false;
        }
        previous_uuid = file_uuid;
        result.push_back(std::move(parsed));
    }
    return true;
}

[[nodiscard]] bool parse_token(const QJsonValue& value, std::optional<QString>& result) {
    if (value.isNull()) {
        result.reset();
        return true;
    }
    if (!value.isString()) {
        return false;
    }
    const auto token = value.toString();
    if (token.isEmpty() || token.size() > kMaximumContinuationTokenCharacters ||
        !std::all_of(token.cbegin(), token.cend(), [](const QChar character) {
            return character.unicode() >= 0x21U && character.unicode() <= 0x7EU;
        })) {
        return false;
    }
    result = token;
    return true;
}

} // namespace

QByteArray encode_service_info_request(const QString& request_id) {
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
                                     {QStringLiteral("request_id"), request_id},
                                     {QStringLiteral("kind"), 1}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_recovery_point_request(const QString& request_id,
                                         const std::optional<QString>& continuation_token) {
    const QJsonObject list{
        {QStringLiteral("maximum_results"), static_cast<qint64>(kRecoveryPointPageSize)},
        {QStringLiteral("continuation_token"),
         continuation_token ? QJsonValue(*continuation_token) : QJsonValue(QJsonValue::Null)}};
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
                                     {QStringLiteral("request_id"), request_id},
                                     {QStringLiteral("kind"), 2},
                                     {QStringLiteral("repository_list"), list}})
        .toJson(QJsonDocument::Compact);
}

bool parse_response_root(const QByteArray& body, const QString& request_id, QJsonObject& root) {
    QJsonParseError parse_error{};
    const auto document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    root = document.object();
    qint64 schema_version = 0;
    return has_exact_keys(root, {"schema_version", "request_id", "kind", "boundary_error_code",
                                 "message_code", "service", "recovery_points"}) &&
           integer_in_range(root.value(QStringLiteral("schema_version")), kServiceSchemaVersion,
                            kServiceSchemaVersion, schema_version) &&
           root.value(QStringLiteral("request_id")).isString() &&
           root.value(QStringLiteral("request_id")).toString() == request_id &&
           root.value(QStringLiteral("message_code")).isString() &&
           stable_code(root.value(QStringLiteral("message_code")).toString(), 128);
}

bool parse_service_info_response(const QJsonObject& root, ServiceInfo& result) {
    qint64 kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        root.value(QStringLiteral("message_code")).toString() != QStringLiteral("service.ready") ||
        !root.value(QStringLiteral("service")).isObject() ||
        !root.value(QStringLiteral("recovery_points")).isNull()) {
        return false;
    }
    const auto service = root.value(QStringLiteral("service")).toObject();
    if (!has_exact_keys(service, {"api_version", "state", "service_version", "capabilities"}) ||
        !service.value(QStringLiteral("service_version")).isString() ||
        !service.value(QStringLiteral("capabilities")).isArray()) {
        return false;
    }
    qint64 api_version = 0;
    qint64 state = 0;
    if (!integer_in_range(service.value(QStringLiteral("api_version")), kServiceApiVersion,
                          kServiceApiVersion, api_version) ||
        !integer_in_range(service.value(QStringLiteral("state")), 2, 2, state)) {
        return false;
    }
    const auto version = service.value(QStringLiteral("service_version")).toString();
    const auto capability_values = service.value(QStringLiteral("capabilities")).toArray();
    if (version.isEmpty() || version.size() > kMaximumVersionCharacters ||
        capability_values.isEmpty() || capability_values.size() > kMaximumCapabilities) {
        return false;
    }
    QStringList capabilities;
    for (const auto& capability : capability_values) {
        if (!capability.isString() ||
            !stable_code(capability.toString(), kMaximumCapabilityCharacters)) {
            return false;
        }
        capabilities.push_back(capability.toString());
    }
    if (!std::is_sorted(capabilities.cbegin(), capabilities.cend()) ||
        std::adjacent_find(capabilities.cbegin(), capabilities.cend()) != capabilities.cend()) {
        return false;
    }
    result = {version, std::move(capabilities)};
    return true;
}

bool parse_recovery_point_response(const QJsonObject& root, RecoveryPointPage& result) {
    qint64 kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 3, 3, kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        !root.value(QStringLiteral("service")).isNull() ||
        !root.value(QStringLiteral("recovery_points")).isObject()) {
        return false;
    }
    const auto page = root.value(QStringLiteral("recovery_points")).toObject();
    if (!has_exact_keys(page, {"state", "repository_uuid", "items", "continuation_token"}) ||
        !page.value(QStringLiteral("repository_uuid")).isString()) {
        return false;
    }
    qint64 state = 0;
    if (!integer_in_range(page.value(QStringLiteral("state")), 1, 2, state) ||
        !parse_items(page.value(QStringLiteral("items")), result.items) ||
        !parse_token(page.value(QStringLiteral("continuation_token")), result.continuation_token)) {
        return false;
    }
    result.configured = state == 2;
    result.repository_uuid = page.value(QStringLiteral("repository_uuid")).toString();
    const auto message = root.value(QStringLiteral("message_code")).toString();
    if (!result.configured) {
        return result.repository_uuid.isEmpty() && result.items.isEmpty() &&
               !result.continuation_token && message == QStringLiteral("repository.not_configured");
    }
    return canonical_uuid(result.repository_uuid) &&
           message == QStringLiteral("repository.catalog_ready") &&
           (!result.items.isEmpty() || !result.continuation_token);
}

bool is_repository_failure_response(const QJsonObject& root) {
    qint64 kind = 0;
    qint64 error = 0;
    return integer_in_range(root.value(QStringLiteral("kind")), 2, 2, kind) &&
           integer_in_range(root.value(QStringLiteral("boundary_error_code")), 1, 11, error) &&
           root.value(QStringLiteral("message_code")).toString() ==
               QStringLiteral("repository.query_failed") &&
           root.value(QStringLiteral("service")).isNull() &&
           root.value(QStringLiteral("recovery_points")).isNull();
}

} // namespace aegra::desktop
