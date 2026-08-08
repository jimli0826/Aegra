#include "client/service_protocol.h"
#include "client/service_protocol_detail.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QVariantMap>

#include <algorithm>
#include <limits>

namespace aegra::desktop {
namespace {

using protocol_detail::has_exact_keys;
using protocol_detail::integer_in_range;
using protocol_detail::parse_display_name;
using protocol_detail::parse_token;
using protocol_detail::stable_code;

[[nodiscard]] QJsonObject make_page(const quint32 maximum_results,
                                    const std::optional<QString>& continuation_token) {
    return QJsonObject{
        {QStringLiteral("maximum_results"), static_cast<qint64>(maximum_results)},
        {QStringLiteral("continuation_token"),
         continuation_token ? QJsonValue(*continuation_token) : QJsonValue(QJsonValue::Null)}};
}

[[nodiscard]] bool parse_optional_string(const QJsonValue& value, std::optional<QString>& result,
                                         const qsizetype maximum_characters) {
    if (value.isNull()) {
        result.reset();
        return true;
    }
    if (!value.isString()) {
        return false;
    }
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > maximum_characters) {
        return false;
    }
    result = text;
    return true;
}

[[nodiscard]] bool parse_file_source_node(const QJsonValue& value, QVariantMap& result) {
    if (!value.isObject()) {
        return false;
    }
    const auto object = value.toObject();
    if (!has_exact_keys(object, {"node_token", "display_name", "entry_kind", "selectability",
                                 "has_children", "is_directory", "availability", "message_code"})) {
        return false;
    }
    const auto token = object.value(QStringLiteral("node_token")).toString();
    QString display_name;
    qint64 entry_kind = 0;
    qint64 selectability = 0;
    qint64 availability = 0;
    if (!object.value(QStringLiteral("node_token")).isString() || !stable_code(token, 512) ||
        !parse_display_name(object.value(QStringLiteral("display_name")), display_name) ||
        !integer_in_range(object.value(QStringLiteral("entry_kind")), 1, 4, entry_kind) ||
        !integer_in_range(object.value(QStringLiteral("selectability")), 1, 3, selectability) ||
        !object.value(QStringLiteral("has_children")).isBool() ||
        !object.value(QStringLiteral("is_directory")).isBool() ||
        !integer_in_range(object.value(QStringLiteral("availability")), 1, 3, availability)) {
        return false;
    }
    QString message_code;
    const auto message_value = object.value(QStringLiteral("message_code"));
    if (!message_value.isNull()) {
        if (!message_value.isString() || !stable_code(message_value.toString(), 128)) {
            return false;
        }
        message_code = message_value.toString();
    }
    result = {{QStringLiteral("nodeToken"), token},
              {QStringLiteral("displayName"), display_name},
              {QStringLiteral("entryKind"), entry_kind},
              {QStringLiteral("selectability"), selectability},
              {QStringLiteral("hasChildren"), object.value(QStringLiteral("has_children")).toBool()},
              {QStringLiteral("isDirectory"), object.value(QStringLiteral("is_directory")).toBool()},
              {QStringLiteral("availability"), availability},
              {QStringLiteral("messageCode"), message_code}};
    return true;
}

[[nodiscard]] bool parse_recover_entry(const QJsonValue& value, QVariantMap& result) {
    if (!value.isObject()) {
        return false;
    }
    const auto object = value.toObject();
    if (!has_exact_keys(object, {"entry_id", "display_name", "entry_kind", "logical_size_bytes",
                                 "has_children", "message_code"})) {
        return false;
    }
    const auto entry_id = object.value(QStringLiteral("entry_id")).toString();
    QString display_name;
    qint64 entry_kind = 0;
    qint64 logical_size = 0;
    if (!object.value(QStringLiteral("entry_id")).isString() || entry_id.isEmpty() ||
        entry_id.size() > 20 ||
        !std::all_of(entry_id.cbegin(), entry_id.cend(),
                     [](const QChar character) {
                         return character.unicode() >= '0' && character.unicode() <= '9';
                     }) ||
        !parse_display_name(object.value(QStringLiteral("display_name")), display_name) ||
        !integer_in_range(object.value(QStringLiteral("entry_kind")), 1, 4, entry_kind) ||
        !integer_in_range(object.value(QStringLiteral("logical_size_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), logical_size) ||
        !object.value(QStringLiteral("has_children")).isBool()) {
        return false;
    }
    QString message_code;
    const auto message_value = object.value(QStringLiteral("message_code"));
    if (!message_value.isNull()) {
        if (!message_value.isString() || !stable_code(message_value.toString(), 128)) {
            return false;
        }
        message_code = message_value.toString();
    }
    result = {{QStringLiteral("entryId"), entry_id},
              {QStringLiteral("displayName"), display_name},
              {QStringLiteral("entryKind"), entry_kind},
              {QStringLiteral("logicalSizeBytes"), logical_size},
              {QStringLiteral("hasChildren"), object.value(QStringLiteral("has_children")).toBool()},
              {QStringLiteral("messageCode"), message_code}};
    return true;
}

} // namespace

QByteArray encode_browse_file_sources_request(const QString& request_id,
                                              const std::optional<QString>& parent_node_token,
                                              const std::optional<QString>& continuation_token,
                                              const bool include_unavailable) {
    const QJsonObject payload{
        {QStringLiteral("parent_node_token"),
         parent_node_token ? QJsonValue(*parent_node_token) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("page"), make_page(kFileBrowsePageSize, continuation_token)},
        {QStringLiteral("include_unavailable"), include_unavailable}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kBrowseFileSourcesRequestKind},
                   {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_list_recovery_point_entries_request(
    const QString& request_id, const QString& repository_connection_id,
    const QString& recovery_point_id, const QString& parent_entry_id,
    const std::optional<QString>& continuation_token, const QString& archive_secret_ref) {
    const QJsonObject payload{
        {QStringLiteral("repository_connection_id"),
         repository_connection_id.isEmpty() ? QJsonValue(QJsonValue::Null)
                                            : QJsonValue(repository_connection_id)},
        {QStringLiteral("recovery_point_id"), recovery_point_id},
        {QStringLiteral("parent_entry_id"), parent_entry_id},
        {QStringLiteral("page"), make_page(kFileBrowsePageSize, continuation_token)},
        {QStringLiteral("archive_secret_ref"),
         archive_secret_ref.isEmpty() ? QJsonValue(QJsonValue::Null)
                                      : QJsonValue(archive_secret_ref)}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kListRecoveryPointEntriesRequestKind},
                   {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_prepare_file_restore_request(const QString& request_id,
                                               const QString& repository_connection_id,
                                               const QString& recovery_point_id,
                                               const QStringList& entry_ids,
                                               const QString& target_node_token,
                                               const int conflict_policy,
                                               const QString& archive_secret_ref,
                                               const bool restore_security) {
    QJsonArray entries;
    for (const auto& entry_id : entry_ids) {
        entries.push_back(entry_id);
    }
    const QJsonObject payload{
        {QStringLiteral("repository_connection_id"),
         repository_connection_id.isEmpty() ? QJsonValue(QJsonValue::Null)
                                            : QJsonValue(repository_connection_id)},
        {QStringLiteral("recovery_point_id"), recovery_point_id},
        {QStringLiteral("entry_ids"), entries},
        {QStringLiteral("target_node_token"), target_node_token},
        {QStringLiteral("conflict_policy"), conflict_policy},
        {QStringLiteral("archive_secret_ref"),
         archive_secret_ref.isEmpty() ? QJsonValue(QJsonValue::Null)
                                      : QJsonValue(archive_secret_ref)},
        {QStringLiteral("restore_security"), restore_security}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kPrepareFileRestoreRequestKind},
                   {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_start_file_restore_request(const QString& request_id,
                                             const QString& idempotency_key,
                                             const QString& preflight_token,
                                             const QString& archive_secret_ref) {
    const QJsonObject payload{{QStringLiteral("preflight_token"), preflight_token},
                              {QStringLiteral("confirmed"), true},
                              {QStringLiteral("archive_secret_ref"),
                               archive_secret_ref.isEmpty()
                                   ? QJsonValue(QJsonValue::Null)
                                   : QJsonValue(archive_secret_ref)}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kStartFileRestoreRequestKind},
                   {QStringLiteral("idempotency_key"), idempotency_key},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_upsert_file_set_schedule_request(
    const QString& request_id, const QString& idempotency_key, const QString& schedule_id,
    const QString& display_name, const bool enabled, const QVariantList& file_selections,
    const QString& repository_connection_id, const int backup_type, const int trigger_kind,
    const int local_minute_of_day, const int weekday_mask, const QString& timezone_id,
    const bool exclude_page_and_hibernation_files, const bool encryption_enabled,
    const QString& archive_password) {
    QJsonArray selections;
    for (const auto& item : file_selections) {
        const auto map = item.toMap();
        selections.push_back(QJsonObject{
            {QStringLiteral("node_token"), map.value(QStringLiteral("nodeToken")).toString()},
            {QStringLiteral("recursion"), map.value(QStringLiteral("recursion")).toInt()},
            {QStringLiteral("display_label"), map.value(QStringLiteral("displayLabel")).toString()}});
    }
    const QJsonObject file_set{
        {QStringLiteral("selections"), selections},
        {QStringLiteral("options"), QJsonObject{{QStringLiteral("unreadable_policy"), 1}}}};
    const QJsonObject protection{{QStringLiteral("content_kind"), 2},
                                 {QStringLiteral("volume_set"), QJsonValue(QJsonValue::Null)},
                                 {QStringLiteral("file_set"), file_set}};
    const QJsonObject trigger{{QStringLiteral("kind"), trigger_kind},
                              {QStringLiteral("local_minute_of_day"), local_minute_of_day},
                              {QStringLiteral("weekday_mask"), weekday_mask},
                              {QStringLiteral("timezone_id"), timezone_id}};
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
        {QStringLiteral("encryption_enabled"), encryption_enabled},
        {QStringLiteral("archive_password"), archive_password}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kUpsertScheduleRequestKind},
                   {QStringLiteral("idempotency_key"), idempotency_key},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

bool parse_browse_file_sources_response(const QJsonObject& root, FileBrowsePage& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")), kBrowseFileSourcesRequestKind,
                          kBrowseFileSourcesRequestKind, request_kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const auto payload = root.value(QStringLiteral("payload")).toObject();
    if (!has_exact_keys(payload, {"items", "continuation_token"}) ||
        !payload.value(QStringLiteral("items")).isArray()) {
        return false;
    }
    const auto items = payload.value(QStringLiteral("items")).toArray();
    if (items.size() > static_cast<qsizetype>(kFileBrowsePageSize)) {
        return false;
    }
    QVariantList parsed_items;
    for (const auto& item : items) {
        QVariantMap map;
        if (!parse_file_source_node(item, map)) {
            return false;
        }
        parsed_items.push_back(std::move(map));
    }
    result.items = std::move(parsed_items);
    return parse_token(payload.value(QStringLiteral("continuation_token")),
                       result.continuation_token);
}

bool parse_list_recovery_point_entries_response(const QJsonObject& root,
                                                RecoveryPointEntryPage& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")),
                          kListRecoveryPointEntriesRequestKind,
                          kListRecoveryPointEntriesRequestKind, request_kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const auto payload = root.value(QStringLiteral("payload")).toObject();
    if (!has_exact_keys(payload, {"repository_connection_id", "recovery_point_id", "parent_entry_id",
                                  "index_generation", "items", "continuation_token"}) ||
        !payload.value(QStringLiteral("items")).isArray()) {
        return false;
    }
    std::optional<QString> connection_id;
    if (!parse_optional_string(payload.value(QStringLiteral("repository_connection_id")),
                               connection_id, 128) ||
        !payload.value(QStringLiteral("recovery_point_id")).isString() ||
        !payload.value(QStringLiteral("parent_entry_id")).isString() ||
        !payload.value(QStringLiteral("index_generation")).isString()) {
        return false;
    }
    result.repository_connection_id = connection_id;
    result.recovery_point_id = payload.value(QStringLiteral("recovery_point_id")).toString();
    result.parent_entry_id = payload.value(QStringLiteral("parent_entry_id")).toString();
    result.index_generation = payload.value(QStringLiteral("index_generation")).toString();
    const auto items = payload.value(QStringLiteral("items")).toArray();
    if (items.size() > static_cast<qsizetype>(kFileBrowsePageSize)) {
        return false;
    }
    QVariantList parsed_items;
    for (const auto& item : items) {
        QVariantMap map;
        if (!parse_recover_entry(item, map)) {
            return false;
        }
        parsed_items.push_back(std::move(map));
    }
    result.items = std::move(parsed_items);
    return parse_token(payload.value(QStringLiteral("continuation_token")),
                       result.continuation_token);
}

bool parse_prepare_file_restore_response(const QJsonObject& root, FileRestorePreflightPage& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")), kPrepareFileRestoreRequestKind,
                          kPrepareFileRestoreRequestKind, request_kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const auto payload = root.value(QStringLiteral("payload")).toObject();
    if (!has_exact_keys(payload, {"preflight_token", "repository_connection_id", "recovery_point_id",
                                  "entry_count", "logical_size_bytes", "target_free_bytes",
                                  "conflict_policy", "expires_utc_ms", "restore_eligible",
                                  "message_code"})) {
        return false;
    }
    qint64 entry_count = 0;
    qint64 logical_size = 0;
    qint64 target_free = 0;
    qint64 conflict_policy = 0;
    qint64 expires = 0;
    std::optional<QString> connection_id;
    if (!payload.value(QStringLiteral("preflight_token")).isString() ||
        !stable_code(payload.value(QStringLiteral("preflight_token")).toString(), 128) ||
        !parse_optional_string(payload.value(QStringLiteral("repository_connection_id")),
                               connection_id, 128) ||
        !payload.value(QStringLiteral("recovery_point_id")).isString() ||
        !integer_in_range(payload.value(QStringLiteral("entry_count")), 1,
                          (std::numeric_limits<qint64>::max)(), entry_count) ||
        !integer_in_range(payload.value(QStringLiteral("logical_size_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), logical_size) ||
        !integer_in_range(payload.value(QStringLiteral("target_free_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), target_free) ||
        !integer_in_range(payload.value(QStringLiteral("conflict_policy")), 1, 3, conflict_policy) ||
        !integer_in_range(payload.value(QStringLiteral("expires_utc_ms")), 0,
                          (std::numeric_limits<qint64>::max)(), expires) ||
        !payload.value(QStringLiteral("restore_eligible")).isBool() ||
        !payload.value(QStringLiteral("message_code")).isString()) {
        return false;
    }
    result.preflight_token = payload.value(QStringLiteral("preflight_token")).toString();
    result.repository_connection_id = connection_id;
    result.recovery_point_id = payload.value(QStringLiteral("recovery_point_id")).toString();
    result.entry_count = static_cast<quint64>(entry_count);
    result.logical_size_bytes = static_cast<quint64>(logical_size);
    result.target_free_bytes = static_cast<quint64>(target_free);
    result.conflict_policy = static_cast<int>(conflict_policy);
    result.expires_utc_ms = static_cast<quint64>(expires);
    result.restore_eligible = payload.value(QStringLiteral("restore_eligible")).toBool();
    result.message_code = payload.value(QStringLiteral("message_code")).toString();
    return true;
}

bool is_browse_file_sources_failure_response(const QJsonObject& root) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    return integer_in_range(root.value(QStringLiteral("kind")), kRequestFailedResponseKind,
                            kRequestFailedResponseKind, kind) &&
           integer_in_range(root.value(QStringLiteral("request_kind")), kBrowseFileSourcesRequestKind,
                            kBrowseFileSourcesRequestKind, request_kind);
}

bool is_list_recovery_point_entries_failure_response(const QJsonObject& root) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    return integer_in_range(root.value(QStringLiteral("kind")), kRequestFailedResponseKind,
                            kRequestFailedResponseKind, kind) &&
           integer_in_range(root.value(QStringLiteral("request_kind")),
                            kListRecoveryPointEntriesRequestKind,
                            kListRecoveryPointEntriesRequestKind, request_kind);
}

bool is_prepare_file_restore_failure_response(const QJsonObject& root) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    return integer_in_range(root.value(QStringLiteral("kind")), kRequestFailedResponseKind,
                            kRequestFailedResponseKind, kind) &&
           integer_in_range(root.value(QStringLiteral("request_kind")),
                            kPrepareFileRestoreRequestKind, kPrepareFileRestoreRequestKind,
                            request_kind);
}

} // namespace aegra::desktop
