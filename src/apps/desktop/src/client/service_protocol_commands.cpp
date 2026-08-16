#include "client/service_protocol.h"
#include "client/service_protocol_detail.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

#include <algorithm>
#include <functional>
#include <limits>

namespace aegra::desktop {
namespace {

using protocol_detail::canonical_uuid;
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
    if (!has_exact_keys(object,
                        {"source_id", "display_name", "kind", "availability", "capacity_bytes",
                         "free_bytes", "disk_capacity_bytes", "is_system", "is_read_only",
                         "is_selectable", "disk_number", "offset_bytes", "mount_letter",
                         "volume_label", "health_status", "partition_style", "media_type"})) {
        return false;
    }
    const auto source_id = object.value(QStringLiteral("source_id")).toString();
    QString display_name;
    qint64 kind = 0;
    qint64 availability = 0;
    qint64 capacity_bytes = 0;
    qint64 free_bytes = 0;
    qint64 disk_capacity_bytes = 0;
    qint64 disk_number = 0;
    qint64 offset_bytes = 0;
    if (!object.value(QStringLiteral("source_id")).isString() || !stable_code(source_id, 128) ||
        !parse_display_name(object.value(QStringLiteral("display_name")), display_name) ||
        !integer_in_range(object.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(object.value(QStringLiteral("availability")), 1, 2, availability) ||
        !integer_in_range(object.value(QStringLiteral("capacity_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), capacity_bytes) ||
        !integer_in_range(object.value(QStringLiteral("free_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), free_bytes) ||
        free_bytes > capacity_bytes ||
        !integer_in_range(object.value(QStringLiteral("disk_capacity_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), disk_capacity_bytes) ||
        !integer_in_range(object.value(QStringLiteral("disk_number")), 0,
                          (std::numeric_limits<qint64>::max)(), disk_number) ||
        !integer_in_range(object.value(QStringLiteral("offset_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), offset_bytes) ||
        !object.value(QStringLiteral("is_system")).isBool() ||
        !object.value(QStringLiteral("is_read_only")).isBool() ||
        !object.value(QStringLiteral("is_selectable")).isBool() ||
        !object.value(QStringLiteral("mount_letter")).isString() ||
        !object.value(QStringLiteral("volume_label")).isString() ||
        !object.value(QStringLiteral("health_status")).isString() ||
        !object.value(QStringLiteral("partition_style")).isString() ||
        !object.value(QStringLiteral("media_type")).isString()) {
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
              {QStringLiteral("freeBytes"), free_bytes},
              {QStringLiteral("diskCapacityBytes"), disk_capacity_bytes},
              {QStringLiteral("isSystem"), object.value(QStringLiteral("is_system")).toBool()},
              {QStringLiteral("isReadOnly"), object.value(QStringLiteral("is_read_only")).toBool()},
              {QStringLiteral("isSelectable"), is_selectable},
              {QStringLiteral("diskNumber"), disk_number},
              {QStringLiteral("offsetBytes"), offset_bytes},
              {QStringLiteral("mountLetter"), object.value(QStringLiteral("mount_letter")).toString()},
              {QStringLiteral("volumeLabel"), object.value(QStringLiteral("volume_label")).toString()},
              {QStringLiteral("healthStatus"),
               object.value(QStringLiteral("health_status")).toString()},
              {QStringLiteral("partitionStyle"),
               object.value(QStringLiteral("partition_style")).toString()},
              {QStringLiteral("mediaType"), object.value(QStringLiteral("media_type")).toString()}};
    return true;
}

[[nodiscard]] bool parse_connection_item(const QJsonValue& value, QVariantMap& result) {
    if (!value.isObject()) {
        return false;
    }
    const auto object = value.toObject();
    // Prefer 6-field summary (with locator). Accept 5-field only if locator is absent so a
    // briefly mismatched Service process does not hard-fail the whole Repository page.
    const bool has_locator_key = object.contains(QStringLiteral("locator"));
    if (has_locator_key) {
        if (!has_exact_keys(object, {"connection_id", "display_name", "locator", "state",
                                     "is_default", "capabilities"})) {
            return false;
        }
    } else if (!has_exact_keys(
                   object, {"connection_id", "display_name", "state", "is_default", "capabilities"})) {
        return false;
    }
    const auto connection_id = object.value(QStringLiteral("connection_id")).toString();
    QString display_name;
    qint64 state = 0;
    QStringList capabilities;
    QString locator;
    if (has_locator_key) {
        const auto locator_value = object.value(QStringLiteral("locator"));
        if (!locator_value.isString() || locator_value.toString().isEmpty() ||
            locator_value.toString().size() > 2048) {
            return false;
        }
        locator = locator_value.toString();
    }
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
              {QStringLiteral("locator"), locator},
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

QByteArray encode_schedule_list_request(const QString& request_id,
                                        const std::optional<QString>& continuation_token) {
    const QJsonObject page{
        {QStringLiteral("maximum_results"), static_cast<qint64>(kSchedulePageSize)},
        {QStringLiteral("continuation_token"),
         continuation_token ? QJsonValue(*continuation_token) : QJsonValue(QJsonValue::Null)}};
    const QJsonObject payload{{QStringLiteral("page"), page},
                              {QStringLiteral("enabled"), QJsonValue(QJsonValue::Null)}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kListSchedulesRequestKind},
                   {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_upsert_schedule_request(const QString& request_id, const QString& idempotency_key,
                                          const QString& schedule_id, const QString& display_name,
                                          const bool enabled, const QVariantList& source_ids,
                                          const QString& repository_connection_id,
                                          const int backup_type, const int trigger_kind,
                                          const QList<int>& local_minutes_of_day,
                                          const int weekday_mask, const QString& timezone_id,
                                          const bool exclude_page_and_hibernation_files,
                                          const bool deduplication_enabled,
                                          const bool encryption_enabled,
                                          const QString& archive_password,
                                          const quint32 day_of_month_mask) {
    QJsonArray minutes_json;
    for (const auto minute : local_minutes_of_day) {
        minutes_json.push_back(minute);
    }
    const QJsonObject trigger{{QStringLiteral("kind"), trigger_kind},
                              {QStringLiteral("local_minutes_of_day"), minutes_json},
                              {QStringLiteral("weekday_mask"), weekday_mask},
                              {QStringLiteral("day_of_month_mask"),
                               static_cast<qint64>(day_of_month_mask)},
                              {QStringLiteral("timezone_id"), timezone_id}};
    const QJsonObject protection{
        {QStringLiteral("content_kind"), 1},
        {QStringLiteral("volume_set"),
         QJsonObject{{QStringLiteral("source_ids"), QJsonArray::fromVariantList(source_ids)}}},
        {QStringLiteral("file_set"), QJsonValue(QJsonValue::Null)}};
    const QJsonObject payload{
        {QStringLiteral("schedule_id"),
         schedule_id.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(schedule_id)},
        {QStringLiteral("display_name"), display_name},
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("protection"), protection},
        {QStringLiteral("repository_connection_id"), repository_connection_id},
        {QStringLiteral("backup_type"), backup_type},
        {QStringLiteral("trigger"), trigger},
        {QStringLiteral("exclude_page_and_hibernation_files"), exclude_page_and_hibernation_files},
        {QStringLiteral("deduplication_enabled"), deduplication_enabled},
        {QStringLiteral("encryption_enabled"), encryption_enabled},
        {QStringLiteral("archive_password"), archive_password}};
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
                                     {QStringLiteral("message_type"), 1},
                                     {QStringLiteral("request_id"), request_id},
                                     {QStringLiteral("kind"), kUpsertScheduleRequestKind},
                                     {QStringLiteral("idempotency_key"), idempotency_key},
                                     {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_delete_schedule_request(const QString& request_id, const QString& idempotency_key,
                                          const QString& schedule_id) {
    return encode_repository_connection_resource_request(request_id, idempotency_key,
                                                         kDeleteScheduleRequestKind, schedule_id);
}

QByteArray
encode_repository_connection_list_request(const QString& request_id,
                                          const std::optional<QString>& continuation_token) {
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
                                       const QString& schedule_id, const int backup_type) {
    const QJsonObject payload{{QStringLiteral("schedule_id"), schedule_id},
                              {QStringLiteral("backup_type"), backup_type}};
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
                                     {QStringLiteral("message_type"), 1},
                                     {QStringLiteral("request_id"), request_id},
                                     {QStringLiteral("kind"), kStartBackupRequestKind},
                                     {QStringLiteral("idempotency_key"), idempotency_key},
                                     {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_prepare_restore_request(const QString& request_id, const QString& connection_id,
                                          const QString& recovery_point_id,
                                          const QString& target_source_id,
                                          const int source_disk_number,
                                          const int source_volume_index,
                                          const QString& archive_password) {
    const QJsonObject payload{{QStringLiteral("repository_connection_id"), connection_id},
                              {QStringLiteral("recovery_point_id"), recovery_point_id},
                              {QStringLiteral("target_source_id"), target_source_id},
                              {QStringLiteral("source_disk_number"), source_disk_number},
                              {QStringLiteral("source_volume_index"), source_volume_index},
                              {QStringLiteral("archive_password"), archive_password}};
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
                                     {QStringLiteral("message_type"), 1},
                                     {QStringLiteral("request_id"), request_id},
                                     {QStringLiteral("kind"), kPrepareRestoreRequestKind},
                                     {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                                     {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_start_restore_request(const QString& request_id, const QString& idempotency_key,
                                        const QString& preflight_token,
                                        const QString& archive_password,
                                        const bool preserve_disk_signature,
                                        const bool auto_expand_last_partition) {
    const QJsonObject payload{
        {QStringLiteral("preflight_token"), preflight_token},
        {QStringLiteral("confirmed"), true},
        {QStringLiteral("archive_password"), archive_password},
        {QStringLiteral("preserve_disk_signature"), preserve_disk_signature},
        {QStringLiteral("auto_expand_last_partition"), auto_expand_last_partition}};
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
                                     {QStringLiteral("message_type"), 1},
                                     {QStringLiteral("request_id"), request_id},
                                     {QStringLiteral("kind"), kStartRestoreRequestKind},
                                     {QStringLiteral("idempotency_key"), idempotency_key},
                                     {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_cancel_job_request(const QString& request_id, const QString& idempotency_key,
                                     const QString& job_id) {
    const QJsonObject payload{{QStringLiteral("resource_id"), job_id}};
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
                                     {QStringLiteral("message_type"), 1},
                                     {QStringLiteral("request_id"), request_id},
                                     {QStringLiteral("kind"), kCancelJobRequestKind},
                                     {QStringLiteral("idempotency_key"), idempotency_key},
                                     {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_mount_session_list_request(const QString& request_id) {
    const QJsonObject page{
        {QStringLiteral("maximum_results"), static_cast<qint64>(kMountSessionPageSize)},
        {QStringLiteral("continuation_token"), QJsonValue(QJsonValue::Null)}};
    const QJsonObject payload{{QStringLiteral("page"), page},
                              {QStringLiteral("state"), QJsonValue(QJsonValue::Null)}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kListMountSessionsRequestKind},
                   {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_mount_recovery_point_request(const QString& request_id,
                                               const QString& idempotency_key,
                                               const QString& connection_id,
                                               const QString& recovery_point_id,
                                               const int source_disk_number,
                                               const QString& preferred_drive_letter,
                                               const QString& archive_password) {
    const QJsonObject payload{
        {QStringLiteral("repository_connection_id"), connection_id},
        {QStringLiteral("recovery_point_id"), recovery_point_id},
        {QStringLiteral("source_disk_number"), source_disk_number},
        {QStringLiteral("preferred_drive_letter"),
         preferred_drive_letter.isEmpty() ? QJsonValue(QJsonValue::Null)
                                          : QJsonValue(preferred_drive_letter)},
        {QStringLiteral("archive_password"), archive_password}};
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
                                     {QStringLiteral("message_type"), 1},
                                     {QStringLiteral("request_id"), request_id},
                                     {QStringLiteral("kind"), kMountRecoveryPointRequestKind},
                                     {QStringLiteral("idempotency_key"), idempotency_key},
                                     {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_unmount_session_request(const QString& request_id, const QString& idempotency_key,
                                          const QString& session_id) {
    const QJsonObject payload{{QStringLiteral("resource_id"), session_id}};
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
                                     {QStringLiteral("message_type"), 1},
                                     {QStringLiteral("request_id"), request_id},
                                     {QStringLiteral("kind"), kUnmountSessionRequestKind},
                                     {QStringLiteral("idempotency_key"), idempotency_key},
                                     {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_repository_connection_input_request(
    const QString& request_id, const QString& idempotency_key, const int request_kind,
    const QString& display_name, const QString& locator, const QString& network_username,
    const QString& network_password, const QString& network_domain) {
    QJsonObject payload{{QStringLiteral("display_name"), display_name},
                        {QStringLiteral("locator"), locator},
                        {QStringLiteral("credential_ref"), QJsonValue(QJsonValue::Null)}};
    if (!network_username.isEmpty() || !network_password.isEmpty() || !network_domain.isEmpty()) {
        payload.insert(QStringLiteral("network_username"), network_username);
        payload.insert(QStringLiteral("network_password"), network_password);
        payload.insert(QStringLiteral("network_domain"), network_domain);
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
                                     {QStringLiteral("message_type"), 1},
                                     {QStringLiteral("request_id"), request_id},
                                     {QStringLiteral("kind"), request_kind},
                                     {QStringLiteral("idempotency_key"), idempotency_key},
                                     {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_repository_connection_resource_request(const QString& request_id,
                                                         const QString& idempotency_key,
                                                         const int request_kind,
                                                         const QString& connection_id) {
    const QJsonObject payload{{QStringLiteral("resource_id"), connection_id}};
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
                                     {QStringLiteral("message_type"), 1},
                                     {QStringLiteral("request_id"), request_id},
                                     {QStringLiteral("kind"), request_kind},
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

[[nodiscard]] bool parse_schedule_item(const QJsonValue& value, QVariantMap& result) {
    if (!value.isObject()) {
        return false;
    }
    const auto object = value.toObject();
    // Must match Service encode_schedule / ScheduleSummary wire fields (schema 4).
    if (!has_exact_keys(object, {"schedule_id", "display_name", "enabled", "content_kind",
                                 "source_ids", "selection_summaries", "repository_connection_id",
                                 "backup_type", "trigger", "next_run_utc_ms",
                                 "exclude_page_and_hibernation_files", "deduplication_enabled",
                                 "encryption_enabled"})) {
        return false;
    }
    const auto schedule_id = object.value(QStringLiteral("schedule_id")).toString();
    const auto source_array = object.value(QStringLiteral("source_ids")).toArray();
    QVariantList source_ids;
    QSet<QString> seen_source_ids;
    for (const auto& source_value : source_array) {
        const auto source_id = source_value.toString();
        if (!source_value.isString() || !stable_code(source_id, 128) ||
            seen_source_ids.contains(source_id)) {
            return false;
        }
        seen_source_ids.insert(source_id);
        source_ids.push_back(source_id);
    }
    QString display_name;
    qint64 content_kind = 0;
    qint64 backup_type = 0;
    const auto summary_array = object.value(QStringLiteral("selection_summaries")).toArray();
    if (!object.value(QStringLiteral("schedule_id")).isString() || !stable_code(schedule_id, 128) ||
        !parse_display_name(object.value(QStringLiteral("display_name")), display_name) ||
        !object.value(QStringLiteral("enabled")).isBool() ||
        !integer_in_range(object.value(QStringLiteral("content_kind")), 1, 2, content_kind) ||
        !object.value(QStringLiteral("source_ids")).isArray() ||
        !object.value(QStringLiteral("selection_summaries")).isArray() ||
        (content_kind == 1 &&
         (source_ids.isEmpty() || source_ids.size() > 100 || !summary_array.isEmpty())) ||
        (content_kind == 2 &&
         (!source_ids.isEmpty() || summary_array.isEmpty() || summary_array.size() > 100)) ||
        !object.value(QStringLiteral("repository_connection_id")).isString() ||
        !stable_code(object.value(QStringLiteral("repository_connection_id")).toString(), 128) ||
        !integer_in_range(object.value(QStringLiteral("backup_type")), 1, 3, backup_type) ||
        // file_set schedules: Full (1) or Incremental (2) only — never Differential.
        (content_kind == 2 && backup_type != 1 && backup_type != 2) ||
        !object.value(QStringLiteral("trigger")).isObject() ||
        !object.value(QStringLiteral("exclude_page_and_hibernation_files")).isBool() ||
        !object.value(QStringLiteral("deduplication_enabled")).isBool() ||
        !object.value(QStringLiteral("encryption_enabled")).isBool() ||
        (content_kind == 2 &&
         object.value(QStringLiteral("deduplication_enabled")).toBool())) {
        return false;
    }
    QVariantList selection_summaries;
    QSet<QString> seen_selection_ids;
    for (const auto& summary_value : summary_array) {
        if (!summary_value.isObject()) {
            return false;
        }
        const auto summary_object = summary_value.toObject();
        if (!has_exact_keys(summary_object, {"selection_id", "display_label", "entry_kind",
                                             "recursion", "display_chain"})) {
            return false;
        }
        const auto selection_id = summary_object.value(QStringLiteral("selection_id")).toString();
        QString display_label;
        qint64 entry_kind = 0;
        qint64 recursion = 0;
        if (!summary_object.value(QStringLiteral("selection_id")).isString() ||
            !stable_code(selection_id, 128) || seen_selection_ids.contains(selection_id) ||
            !parse_display_name(summary_object.value(QStringLiteral("display_label")),
                                display_label) ||
            !integer_in_range(summary_object.value(QStringLiteral("entry_kind")), 1, 4,
                              entry_kind) ||
            !integer_in_range(summary_object.value(QStringLiteral("recursion")), 1, 2, recursion) ||
            !summary_object.value(QStringLiteral("display_chain")).isArray()) {
            return false;
        }
        QStringList display_chain;
        const auto chain_array = summary_object.value(QStringLiteral("display_chain")).toArray();
        if (chain_array.isEmpty() || chain_array.size() > 64) {
            return false;
        }
        for (const auto& part_value : chain_array) {
            QString part;
            if (!parse_display_name(part_value, part)) {
                return false;
            }
            display_chain.push_back(std::move(part));
        }
        seen_selection_ids.insert(selection_id);
        selection_summaries.push_back(QVariantMap{
            {QStringLiteral("selectionId"), selection_id},
            {QStringLiteral("displayLabel"), display_label},
            {QStringLiteral("entryKind"), entry_kind},
            {QStringLiteral("recursion"), recursion},
            {QStringLiteral("displayChain"), display_chain}});
    }
    const auto trigger = object.value(QStringLiteral("trigger")).toObject();
    if (!has_exact_keys(trigger, {"kind", "local_minutes_of_day", "weekday_mask",
                                  "day_of_month_mask", "timezone_id"})) {
        return false;
    }
    qint64 trigger_kind = 0;
    qint64 weekday_mask = 0;
    qint64 day_of_month_mask = 0;
    if (!integer_in_range(trigger.value(QStringLiteral("kind")), 1, 3, trigger_kind) ||
        !trigger.value(QStringLiteral("local_minutes_of_day")).isArray() ||
        !integer_in_range(trigger.value(QStringLiteral("weekday_mask")), 0, 127, weekday_mask) ||
        !integer_in_range(trigger.value(QStringLiteral("day_of_month_mask")), 0, 2147483647,
                          day_of_month_mask) ||
        !trigger.value(QStringLiteral("timezone_id")).isString()) {
        return false;
    }
    const auto minutes_array = trigger.value(QStringLiteral("local_minutes_of_day")).toArray();
    if (minutes_array.isEmpty() || minutes_array.size() > 8) {
        return false;
    }
    QStringList time_labels;
    QVariantList times_of_day;
    QSet<int> seen_minutes;
    for (const auto& minute_value : minutes_array) {
        qint64 minute = 0;
        if (!integer_in_range(minute_value, 0, 24 * 60 - 1, minute) ||
            seen_minutes.contains(static_cast<int>(minute))) {
            return false;
        }
        seen_minutes.insert(static_cast<int>(minute));
        const auto hour = static_cast<int>(minute / 60);
        const auto min = static_cast<int>(minute % 60);
        const auto label = QStringLiteral("%1:%2")
                               .arg(hour, 2, 10, QLatin1Char('0'))
                               .arg(min, 2, 10, QLatin1Char('0'));
        time_labels.push_back(label);
        times_of_day.push_back(label);
    }
    // Sort labels and keep timesOfDay in the same minute order.
    std::sort(time_labels.begin(), time_labels.end());
    times_of_day.clear();
    for (const auto& label : time_labels) {
        times_of_day.push_back(label);
    }
    qint64 next_run = 0;
    const auto next_value = object.value(QStringLiteral("next_run_utc_ms"));
    const bool has_next = !next_value.isNull();
    if (has_next && !integer_in_range(next_value, 0, (std::numeric_limits<qint64>::max)(), next_run)) {
        return false;
    }
    QString frequency = QStringLiteral("daily");
    if (trigger_kind == kScheduleTriggerWeekly) {
        frequency = QStringLiteral("weekly");
    } else if (trigger_kind == kScheduleTriggerMonthly) {
        frequency = QStringLiteral("monthly");
    }
    result = {{QStringLiteral("scheduleId"), schedule_id},
              {QStringLiteral("id"), schedule_id},
              {QStringLiteral("displayName"), display_name},
              {QStringLiteral("sourceName"), display_name},
              {QStringLiteral("enabled"), object.value(QStringLiteral("enabled")).toBool()},
              {QStringLiteral("contentKind"), content_kind},
              {QStringLiteral("sourceIds"), source_ids},
              {QStringLiteral("selectionSummaries"), selection_summaries},
              {QStringLiteral("connectionId"),
               object.value(QStringLiteral("repository_connection_id")).toString()},
              {QStringLiteral("backupType"), backup_type},
              {QStringLiteral("frequency"), frequency},
              {QStringLiteral("timeOfDay"), time_labels.join(QStringLiteral(", "))},
              {QStringLiteral("timesOfDay"), times_of_day},
              {QStringLiteral("weekdayMask"), weekday_mask},
              {QStringLiteral("dayOfMonthMask"), day_of_month_mask},
              {QStringLiteral("timezoneId"), trigger.value(QStringLiteral("timezone_id")).toString()},
              {QStringLiteral("nextRunUtcMs"), has_next ? next_run : QVariant{}},
              {QStringLiteral("excludePageAndHibernation"),
               object.value(QStringLiteral("exclude_page_and_hibernation_files")).toBool()},
              {QStringLiteral("deduplicationEnabled"),
               object.value(QStringLiteral("deduplication_enabled")).toBool()},
              {QStringLiteral("encryptionEnabled"),
               object.value(QStringLiteral("encryption_enabled")).toBool()},
              {QStringLiteral("lastRun"), QString{}},
              {QStringLiteral("destinationName"), QString{}},
              {QStringLiteral("destinationPath"), QString{}}};
    return true;
}

bool parse_schedule_list_response(const QJsonObject& root, SchedulePage& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")), kListSchedulesRequestKind,
                          kListSchedulesRequestKind, request_kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const auto payload = root.value(QStringLiteral("payload")).toObject();
    if (!has_exact_keys(payload, {"items", "continuation_token"})) {
        return false;
    }
    // Service lists schedules by created_utc_ms DESC (then schedule_id ASC within a
    // timestamp), not by schedule_id globally — only reject duplicates.
    return parse_paged_items(payload.value(QStringLiteral("items")), kSchedulePageSize,
                             parse_schedule_item, "scheduleId", false, result.items) &&
           parse_token(payload.value(QStringLiteral("continuation_token")),
                       result.continuation_token);
}

bool is_schedule_list_failure_response(const QJsonObject& root) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    return integer_in_range(root.value(QStringLiteral("kind")), kRequestFailedResponseKind,
                            kRequestFailedResponseKind, kind) &&
           integer_in_range(root.value(QStringLiteral("request_kind")), kListSchedulesRequestKind,
                            kListSchedulesRequestKind, request_kind) &&
           integer_in_range(root.value(QStringLiteral("boundary_error_code")), 1, 11, error) &&
           root.value(QStringLiteral("payload")).isNull();
}

[[nodiscard]] bool parse_mount_session_item(const QJsonValue& value, QVariantMap& result) {
    if (!value.isObject()) {
        return false;
    }
    const auto object = value.toObject();
    if (!has_exact_keys(object,
                        {"session_id", "recovery_point_id", "state", "mount_point",
                         "source_disk_number", "disk_size_bytes", "started_utc_ms",
                         "message_code"})) {
        return false;
    }
    qint64 state = 0;
    qint64 started = 0;
    qint64 source_disk = 0;
    qint64 disk_size = 0;
    const auto session_id = object.value(QStringLiteral("session_id")).toString();
    const auto recovery_point_id = object.value(QStringLiteral("recovery_point_id")).toString();
    const auto message_code = object.value(QStringLiteral("message_code")).toString();
    if (!object.value(QStringLiteral("session_id")).isString() || !stable_code(session_id, 128) ||
        !object.value(QStringLiteral("recovery_point_id")).isString() ||
        !canonical_uuid(recovery_point_id) ||
        !integer_in_range(object.value(QStringLiteral("state")), kMountSessionStateMounting,
                          kMountSessionStateFailed, state) ||
        !object.value(QStringLiteral("mount_point")).isString() ||
        !integer_in_range(object.value(QStringLiteral("source_disk_number")), 0,
                          (std::numeric_limits<qint64>::max)(), source_disk) ||
        !integer_in_range(object.value(QStringLiteral("disk_size_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), disk_size) ||
        !integer_in_range(object.value(QStringLiteral("started_utc_ms")), 0,
                          (std::numeric_limits<qint64>::max)(), started) ||
        !object.value(QStringLiteral("message_code")).isString() ||
        !stable_code(message_code, 128)) {
        return false;
    }
    result = {{QStringLiteral("sessionId"), session_id},
              {QStringLiteral("recoveryPointId"), recovery_point_id},
              {QStringLiteral("state"), state},
              {QStringLiteral("mountPoint"), object.value(QStringLiteral("mount_point")).toString()},
              {QStringLiteral("sourceDiskNumber"), source_disk},
              {QStringLiteral("diskSizeBytes"), disk_size},
              {QStringLiteral("startedUtcMs"), started},
              {QStringLiteral("messageCode"), message_code}};
    return true;
}

bool parse_mount_session_list_response(const QJsonObject& root, MountSessionPage& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")), kListMountSessionsRequestKind,
                          kListMountSessionsRequestKind, request_kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const auto payload = root.value(QStringLiteral("payload")).toObject();
    if (!has_exact_keys(payload, {"items", "continuation_token"})) {
        return false;
    }
    // Service sorts by started_utc_ms (newest first); session_id is not wire-ordered.
    return parse_paged_items(payload.value(QStringLiteral("items")), kMountSessionPageSize,
                             parse_mount_session_item, "sessionId", false, result.items) &&
           parse_token(payload.value(QStringLiteral("continuation_token")),
                       result.continuation_token);
}

bool is_mount_list_failure_response(const QJsonObject& root) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    return integer_in_range(root.value(QStringLiteral("kind")), kRequestFailedResponseKind,
                            kRequestFailedResponseKind, kind) &&
           integer_in_range(root.value(QStringLiteral("request_kind")),
                            kListMountSessionsRequestKind, kListMountSessionsRequestKind,
                            request_kind) &&
           integer_in_range(root.value(QStringLiteral("boundary_error_code")), 1, 11, error) &&
           root.value(QStringLiteral("payload")).isNull();
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
    if (!has_exact_keys(payload, {"command_id", "disposition", "resource_id", "event_subscription",
                                  "free_bytes"})) {
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
    std::optional<qint64> free_bytes;
    const auto free_value = payload.value(QStringLiteral("free_bytes"));
    if (free_value.isNull()) {
        free_bytes = std::nullopt;
    } else if (free_value.isDouble() || free_value.isString()) {
        qint64 free = 0;
        if (!integer_in_range(free_value, 0, (std::numeric_limits<qint64>::max)(), free)) {
            return false;
        }
        free_bytes = free;
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
    result = {command_id, disposition, resource_id, has_resource, message, free_bytes};
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

QByteArray encode_plan_delete_recovery_points_request(const QString& request_id,
                                                      const QString& connection_id,
                                                      const QString& recovery_point_id,
                                                      const QString& archive_password) {
    const QJsonObject payload{{QStringLiteral("repository_connection_id"), connection_id},
                              {QStringLiteral("recovery_point_id"), recovery_point_id},
                              {QStringLiteral("archive_password"), archive_password}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kPlanDeleteRecoveryPointsRequestKind},
                   {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_execute_delete_plan_request(const QString& request_id,
                                              const QString& idempotency_key,
                                              const QString& plan_token, const bool confirmed) {
    const QJsonObject payload{{QStringLiteral("plan_token"), plan_token},
                              {QStringLiteral("confirmed"), confirmed}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kExecuteDeletePlanRequestKind},
                   {QStringLiteral("idempotency_key"), idempotency_key},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

} // namespace aegra::desktop
