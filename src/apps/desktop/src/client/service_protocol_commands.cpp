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
                         "is_selectable", "disk_number", "mount_letter", "volume_label",
                         "health_status", "partition_style", "media_type"})) {
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
                                          const int local_minute_of_day, const int weekday_mask,
                                          const QString& timezone_id,
                                          const bool exclude_page_and_hibernation_files) {
    const QJsonObject trigger{{QStringLiteral("kind"), trigger_kind},
                              {QStringLiteral("local_minute_of_day"), local_minute_of_day},
                              {QStringLiteral("weekday_mask"), weekday_mask},
                              {QStringLiteral("timezone_id"), timezone_id}};
    const QJsonObject payload{
        {QStringLiteral("schedule_id"),
         schedule_id.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(schedule_id)},
        {QStringLiteral("display_name"), display_name},
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("source_ids"), QJsonArray::fromVariantList(source_ids)},
        {QStringLiteral("repository_connection_id"), repository_connection_id},
        {QStringLiteral("backup_type"), backup_type},
        {QStringLiteral("trigger"), trigger},
        {QStringLiteral("exclude_page_and_hibernation_files"), exclude_page_and_hibernation_files}};
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
                                       const QVariantList& source_ids,
                                       const QString& repository_connection_id,
                                       const int backup_type,
                                       const QString& parent_recovery_point_id,
                                       const bool exclude_page_and_hibernation_files) {
    const QJsonObject payload{
        {QStringLiteral("source_ids"), QJsonArray::fromVariantList(source_ids)},
        {QStringLiteral("repository_connection_id"), repository_connection_id},
        {QStringLiteral("backup_type"), backup_type},
        {QStringLiteral("parent_recovery_point_id"), parent_recovery_point_id.isEmpty()
                                                         ? QJsonValue(QJsonValue::Null)
                                                         : QJsonValue(parent_recovery_point_id)},
        {QStringLiteral("exclude_page_and_hibernation_files"), exclude_page_and_hibernation_files}};
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
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
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"),
                                      static_cast<qint64>(kServiceSchemaVersion)},
                                     {QStringLiteral("message_type"), 1},
                                     {QStringLiteral("request_id"), request_id},
                                     {QStringLiteral("kind"), kCancelJobRequestKind},
                                     {QStringLiteral("idempotency_key"), idempotency_key},
                                     {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_repository_connection_input_request(const QString& request_id,
                                                      const QString& idempotency_key,
                                                      const int request_kind,
                                                      const QString& display_name,
                                                      const QString& locator) {
    const QJsonObject payload{{QStringLiteral("display_name"), display_name},
                              {QStringLiteral("locator"), locator},
                              {QStringLiteral("credential_ref"), QJsonValue(QJsonValue::Null)}};
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
    // Must match Service encode_schedule / ScheduleSummary wire fields (9 keys).
    if (!has_exact_keys(object, {"schedule_id", "display_name", "enabled", "source_ids",
                                 "repository_connection_id", "backup_type", "trigger",
                                 "next_run_utc_ms", "exclude_page_and_hibernation_files"})) {
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
    qint64 backup_type = 0;
    if (!object.value(QStringLiteral("schedule_id")).isString() || !stable_code(schedule_id, 128) ||
        !parse_display_name(object.value(QStringLiteral("display_name")), display_name) ||
        !object.value(QStringLiteral("enabled")).isBool() ||
        !object.value(QStringLiteral("source_ids")).isArray() || source_ids.isEmpty() ||
        source_ids.size() > 100 ||
        !object.value(QStringLiteral("repository_connection_id")).isString() ||
        !stable_code(object.value(QStringLiteral("repository_connection_id")).toString(), 128) ||
        !integer_in_range(object.value(QStringLiteral("backup_type")), 1, 3, backup_type) ||
        !object.value(QStringLiteral("trigger")).isObject() ||
        !object.value(QStringLiteral("exclude_page_and_hibernation_files")).isBool()) {
        return false;
    }
    const auto trigger = object.value(QStringLiteral("trigger")).toObject();
    if (!has_exact_keys(trigger,
                        {"kind", "local_minute_of_day", "weekday_mask", "timezone_id"})) {
        return false;
    }
    qint64 trigger_kind = 0;
    qint64 local_minute = 0;
    qint64 weekday_mask = 0;
    if (!integer_in_range(trigger.value(QStringLiteral("kind")), 1, 2, trigger_kind) ||
        !integer_in_range(trigger.value(QStringLiteral("local_minute_of_day")), 0, 24 * 60 - 1,
                          local_minute) ||
        !integer_in_range(trigger.value(QStringLiteral("weekday_mask")), 0, 127, weekday_mask) ||
        !trigger.value(QStringLiteral("timezone_id")).isString()) {
        return false;
    }
    qint64 next_run = 0;
    const auto next_value = object.value(QStringLiteral("next_run_utc_ms"));
    const bool has_next = !next_value.isNull();
    if (has_next && !integer_in_range(next_value, 0, (std::numeric_limits<qint64>::max)(), next_run)) {
        return false;
    }
    const auto hour = static_cast<int>(local_minute / 60);
    const auto minute = static_cast<int>(local_minute % 60);
    const QString time_of_day =
        QStringLiteral("%1:%2")
            .arg(hour, 2, 10, QLatin1Char('0'))
            .arg(minute, 2, 10, QLatin1Char('0'));
    const QString frequency =
        trigger_kind == kScheduleTriggerWeekly ? QStringLiteral("weekly") : QStringLiteral("daily");
    result = {{QStringLiteral("scheduleId"), schedule_id},
              {QStringLiteral("id"), schedule_id},
              {QStringLiteral("displayName"), display_name},
              {QStringLiteral("sourceName"), display_name},
              {QStringLiteral("enabled"), object.value(QStringLiteral("enabled")).toBool()},
              {QStringLiteral("sourceIds"), source_ids},
              {QStringLiteral("connectionId"),
               object.value(QStringLiteral("repository_connection_id")).toString()},
              {QStringLiteral("backupType"), backup_type},
              {QStringLiteral("frequency"), frequency},
              {QStringLiteral("timeOfDay"), time_of_day},
              {QStringLiteral("weekdayMask"), weekday_mask},
              {QStringLiteral("timezoneId"), trigger.value(QStringLiteral("timezone_id")).toString()},
              {QStringLiteral("nextRunUtcMs"), has_next ? next_run : QVariant{}},
              {QStringLiteral("excludePageAndHibernation"),
               object.value(QStringLiteral("exclude_page_and_hibernation_files")).toBool()},
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
    return parse_paged_items(payload.value(QStringLiteral("items")), kSchedulePageSize,
                             parse_schedule_item, "scheduleId", true, result.items) &&
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
