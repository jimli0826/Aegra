#include "client/service_protocol.h"
#include "client/service_protocol_detail.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>
#include <QVariantMap>

#include <algorithm>
#include <functional>
#include <limits>

namespace aegra::desktop {
namespace {

using protocol_detail::has_exact_keys;
using protocol_detail::integer_in_range;
using protocol_detail::kMaximumCapabilities;
using protocol_detail::kMaximumCapabilityCharacters;
using protocol_detail::parse_display_name;
using protocol_detail::parse_token;
using protocol_detail::stable_code;

[[nodiscard]] bool parse_capability_list(const QJsonValue& value, QStringList& result) {
    if (!value.isArray()) {
        return false;
    }
    const auto items = value.toArray();
    if (items.size() > kMaximumCapabilities) {
        return false;
    }
    QStringList capabilities;
    for (const auto& item : items) {
        if (!item.isString() || !stable_code(item.toString(), kMaximumCapabilityCharacters)) {
            return false;
        }
        capabilities.push_back(item.toString());
    }
    if (!std::is_sorted(capabilities.cbegin(), capabilities.cend()) ||
        std::adjacent_find(capabilities.cbegin(), capabilities.cend()) != capabilities.cend()) {
        return false;
    }
    result = std::move(capabilities);
    return true;
}

[[nodiscard]] bool parse_source_item(const QJsonValue& value, QVariantMap& result) {
    if (!value.isObject()) {
        return false;
    }
    const auto object = value.toObject();
    if (!has_exact_keys(object, {"source_id", "display_name", "kind", "availability",
                                 "capacity_bytes", "is_system", "is_read_only", "is_selectable"})) {
        return false;
    }
    const auto source_id = object.value(QStringLiteral("source_id")).toString();
    QString display_name;
    qint64 kind = 0;
    qint64 availability = 0;
    qint64 capacity_bytes = 0;
    if (!object.value(QStringLiteral("source_id")).isString() || !stable_code(source_id, 128) ||
        !parse_display_name(object.value(QStringLiteral("display_name")), display_name) ||
        !integer_in_range(object.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(object.value(QStringLiteral("availability")), 1, 2, availability) ||
        !integer_in_range(object.value(QStringLiteral("capacity_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), capacity_bytes) ||
        !object.value(QStringLiteral("is_system")).isBool() ||
        !object.value(QStringLiteral("is_read_only")).isBool() ||
        !object.value(QStringLiteral("is_selectable")).isBool()) {
        return false;
    }
    const auto is_selectable = object.value(QStringLiteral("is_selectable")).toBool();
    if (is_selectable && availability != 1) {
        return false;
    }
    result = {{QStringLiteral("sourceId"), source_id},
              {QStringLiteral("displayName"), display_name},
              {QStringLiteral("kind"), kind},
              {QStringLiteral("availability"), availability},
              {QStringLiteral("capacityBytes"), capacity_bytes},
              {QStringLiteral("isSystem"), object.value(QStringLiteral("is_system")).toBool()},
              {QStringLiteral("isReadOnly"), object.value(QStringLiteral("is_read_only")).toBool()},
              {QStringLiteral("isSelectable"), is_selectable}};
    return true;
}

[[nodiscard]] bool parse_connection_item(const QJsonValue& value, QVariantMap& result) {
    if (!value.isObject()) {
        return false;
    }
    const auto object = value.toObject();
    if (!has_exact_keys(object,
                        {"connection_id", "display_name", "state", "is_default", "capabilities"})) {
        return false;
    }
    const auto connection_id = object.value(QStringLiteral("connection_id")).toString();
    QString display_name;
    qint64 state = 0;
    QStringList capabilities;
    if (!object.value(QStringLiteral("connection_id")).isString() ||
        !stable_code(connection_id, 128) ||
        !parse_display_name(object.value(QStringLiteral("display_name")), display_name) ||
        !integer_in_range(object.value(QStringLiteral("state")), 1, 2, state) ||
        !object.value(QStringLiteral("is_default")).isBool() ||
        !parse_capability_list(object.value(QStringLiteral("capabilities")), capabilities)) {
        return false;
    }
    result = {{QStringLiteral("connectionId"), connection_id},
              {QStringLiteral("displayName"), display_name},
              {QStringLiteral("state"), state},
              {QStringLiteral("isDefault"), object.value(QStringLiteral("is_default")).toBool()},
              {QStringLiteral("capabilities"), capabilities}};
    return true;
}

// require_increasing: inventory is sorted by source_id; connection pages are only
// duplicate-free (ordered by created time, not connection_id).
[[nodiscard]] bool
parse_paged_items(const QJsonValue& value, const quint32 page_size,
                  const std::function<bool(const QJsonValue&, QVariantMap&)>& parse,
                  const char* id_key, const bool require_increasing, QVariantList& result) {
    if (!value.isArray()) {
        return false;
    }
    const auto items = value.toArray();
    if (items.size() > static_cast<qsizetype>(page_size)) {
        return false;
    }
    QVariantList parsed_items;
    QString previous_id;
    for (const auto& item : items) {
        QVariantMap parsed;
        if (!parse(item, parsed)) {
            return false;
        }
        const auto id = parsed.value(QLatin1String(id_key)).toString();
        if (!previous_id.isEmpty()) {
            if (require_increasing) {
                if (id <= previous_id) {
                    return false;
                }
            } else if (id == previous_id) {
                return false;
            }
        }
        previous_id = id;
        parsed_items.push_back(std::move(parsed));
    }
    result = std::move(parsed_items);
    return true;
}

} // namespace

QByteArray encode_source_inventory_request(const QString& request_id,
                                           const std::optional<QString>& continuation_token,
                                           const bool include_unavailable) {
    const QJsonObject page{
        {QStringLiteral("maximum_results"), static_cast<qint64>(kInventoryPageSize)},
        {QStringLiteral("continuation_token"),
         continuation_token ? QJsonValue(*continuation_token) : QJsonValue(QJsonValue::Null)}};
    const QJsonObject payload{{QStringLiteral("page"), page},
                              {QStringLiteral("include_unavailable"), include_unavailable}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kListSourceInventoryRequestKind},
                   {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_repository_connection_list_request(
    const QString& request_id, const std::optional<QString>& continuation_token) {
    const QJsonObject page{
        {QStringLiteral("maximum_results"), static_cast<qint64>(kConnectionPageSize)},
        {QStringLiteral("continuation_token"),
         continuation_token ? QJsonValue(*continuation_token) : QJsonValue(QJsonValue::Null)}};
    const QJsonObject payload{{QStringLiteral("page"), page},
                              {QStringLiteral("state"), QJsonValue(QJsonValue::Null)}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kListRepositoryConnectionsRequestKind},
                   {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_start_backup_request(const QString& request_id, const QString& idempotency_key,
                                       const QString& source_id,
                                       const QString& repository_connection_id,
                                       const int backup_type,
                                       const QString& parent_recovery_point_id) {
    const QJsonObject payload{
        {QStringLiteral("source_id"), source_id},
        {QStringLiteral("repository_connection_id"), repository_connection_id},
        {QStringLiteral("backup_type"), backup_type},
        {QStringLiteral("parent_recovery_point_id"),
         parent_recovery_point_id.isEmpty() ? QJsonValue(QJsonValue::Null)
                                            : QJsonValue(parent_recovery_point_id)}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kStartBackupRequestKind},
                   {QStringLiteral("idempotency_key"), idempotency_key},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_cancel_job_request(const QString& request_id, const QString& idempotency_key,
                                     const QString& job_id) {
    const QJsonObject payload{{QStringLiteral("resource_id"), job_id}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kCancelJobRequestKind},
                   {QStringLiteral("idempotency_key"), idempotency_key},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

bool parse_source_inventory_response(const QJsonObject& root, SourceInventoryPage& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")),
                          kListSourceInventoryRequestKind, kListSourceInventoryRequestKind,
                          request_kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const auto payload = root.value(QStringLiteral("payload")).toObject();
    if (!has_exact_keys(payload, {"items", "continuation_token"})) {
        return false;
    }
    return parse_paged_items(payload.value(QStringLiteral("items")), kInventoryPageSize,
                             parse_source_item, "sourceId", true, result.items) &&
           parse_token(payload.value(QStringLiteral("continuation_token")),
                       result.continuation_token);
}

bool parse_repository_connection_list_response(const QJsonObject& root,
                                               RepositoryConnectionPage& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")),
                          kListRepositoryConnectionsRequestKind,
                          kListRepositoryConnectionsRequestKind, request_kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const auto payload = root.value(QStringLiteral("payload")).toObject();
    if (!has_exact_keys(payload, {"items", "continuation_token"})) {
        return false;
    }
    return parse_paged_items(payload.value(QStringLiteral("items")), kConnectionPageSize,
                             parse_connection_item, "connectionId", false, result.items) &&
           parse_token(payload.value(QStringLiteral("continuation_token")),
                       result.continuation_token);
}

bool parse_command_ack_response(const QJsonObject& root, const int expected_request_kind,
                                CommandAck& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    qint64 disposition = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), kCommandAcceptedResponseKind,
                          kCommandAcceptedResponseKind, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")), expected_request_kind,
                          expected_request_kind, request_kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const auto payload = root.value(QStringLiteral("payload")).toObject();
    if (!has_exact_keys(payload,
                        {"command_id", "disposition", "resource_id", "event_subscription"})) {
        return false;
    }
    const auto command_id = payload.value(QStringLiteral("command_id")).toString();
    if (!payload.value(QStringLiteral("command_id")).isString() || !stable_code(command_id, 128) ||
        !integer_in_range(payload.value(QStringLiteral("disposition")), kCommandDispositionAccepted,
                          kCommandDispositionReplayed, disposition) ||
        !payload.value(QStringLiteral("event_subscription")).isNull()) {
        return false;
    }
    QString resource_id;
    bool has_resource = false;
    const auto resource_value = payload.value(QStringLiteral("resource_id"));
    if (resource_value.isNull()) {
        has_resource = false;
    } else if (resource_value.isString() && stable_code(resource_value.toString(), 128)) {
        resource_id = resource_value.toString();
        has_resource = true;
    } else {
        return false;
    }
    const auto message = root.value(QStringLiteral("message_code")).toString();
    if ((disposition == kCommandDispositionAccepted &&
         message != QLatin1String("command.accepted")) ||
        (disposition == kCommandDispositionReplayed &&
         message != QLatin1String("command.replayed"))) {
        return false;
    }
    result = {command_id, disposition, resource_id, has_resource, message};
    return true;
}

bool is_inventory_failure_response(const QJsonObject& root) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    return integer_in_range(root.value(QStringLiteral("kind")), kRequestFailedResponseKind,
                            kRequestFailedResponseKind, kind) &&
           integer_in_range(root.value(QStringLiteral("request_kind")),
                            kListSourceInventoryRequestKind, kListSourceInventoryRequestKind,
                            request_kind) &&
           integer_in_range(root.value(QStringLiteral("boundary_error_code")), 1, 11, error) &&
           root.value(QStringLiteral("payload")).isNull();
}

bool is_connection_list_failure_response(const QJsonObject& root) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    return integer_in_range(root.value(QStringLiteral("kind")), kRequestFailedResponseKind,
                            kRequestFailedResponseKind, kind) &&
           integer_in_range(root.value(QStringLiteral("request_kind")),
                            kListRepositoryConnectionsRequestKind,
                            kListRepositoryConnectionsRequestKind, request_kind) &&
           integer_in_range(root.value(QStringLiteral("boundary_error_code")), 1, 11, error) &&
           root.value(QStringLiteral("payload")).isNull();
}

bool is_command_failure_response(const QJsonObject& root, const int expected_request_kind) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    return integer_in_range(root.value(QStringLiteral("kind")), kRequestFailedResponseKind,
                            kRequestFailedResponseKind, kind) &&
           integer_in_range(root.value(QStringLiteral("request_kind")), expected_request_kind,
                            expected_request_kind, request_kind) &&
           integer_in_range(root.value(QStringLiteral("boundary_error_code")), 1, 11, error) &&
           root.value(QStringLiteral("payload")).isNull();
}

} // namespace aegra::desktop
