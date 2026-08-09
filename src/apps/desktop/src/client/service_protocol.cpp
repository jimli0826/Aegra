#include "client/service_protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <limits>

namespace aegra::desktop {
namespace {

constexpr qsizetype kMaximumVersionCharacters = 64;
constexpr qsizetype kMaximumCapabilities = 64;
constexpr qsizetype kMaximumCapabilityCharacters = 64;
constexpr qsizetype kMaximumContinuationTokenCharacters = 1'024;
constexpr qsizetype kMaximumStableCodeCharacters = 128;

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

[[nodiscard]] bool valid_message_arguments(const QJsonValue& value) {
    if (!value.isArray() || value.toArray().size() > 16) {
        return false;
    }
    QString previous;
    for (const auto& item : value.toArray()) {
        if (!item.isObject()) {
            return false;
        }
        const auto argument = item.toObject();
        if (!has_exact_keys(argument, {"name", "value"}) ||
            !argument.value(QStringLiteral("name")).isString() ||
            !stable_code(argument.value(QStringLiteral("name")).toString(), 64) ||
            !argument.value(QStringLiteral("value")).isString()) {
            return false;
        }
        const auto name = argument.value(QStringLiteral("name")).toString();
        const auto text = argument.value(QStringLiteral("value")).toString();
        if ((!previous.isEmpty() && name <= previous) || text.isEmpty() || text.size() > 256 ||
            std::any_of(text.cbegin(), text.cend(), [](const QChar character) {
                return character.unicode() < 0x20U || character.unicode() == 0x7FU;
            })) {
            return false;
        }
        previous = name;
    }
    return true;
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
                                 "content_kind", "chain_state", "created_utc_ms",
                                 "logical_size_bytes", "stored_size_bytes",
                                 "deduplicated_block_count", "deduplicated_logical_bytes",
                                 "source_count", "has_sidecar"})) {
        return false;
    }
    const auto file_uuid = object.value(QStringLiteral("file_uuid")).toString();
    const auto backup_set_uuid = object.value(QStringLiteral("backup_set_uuid")).toString();
    QString parent_uuid;
    bool has_parent = false;
    qint64 backup_type = 0;
    qint64 content_kind = 0;
    qint64 chain_state = 0;
    qint64 created_utc_ms = 0;
    qint64 logical_size_bytes = 0;
    qint64 stored_size_bytes = 0;
    qint64 deduplicated_block_count = 0;
    qint64 deduplicated_logical_bytes = 0;
    qint64 source_count = 0;
    if (!canonical_uuid(file_uuid) || !canonical_uuid(backup_set_uuid) ||
        !optional_uuid(object.value(QStringLiteral("parent_uuid")), parent_uuid, has_parent) ||
        !integer_in_range(object.value(QStringLiteral("backup_type")), 1, 3, backup_type) ||
        !integer_in_range(object.value(QStringLiteral("content_kind")), 1, 2, content_kind) ||
        !integer_in_range(object.value(QStringLiteral("chain_state")), 1, 2, chain_state) ||
        !integer_in_range(object.value(QStringLiteral("created_utc_ms")), 0,
                          (std::numeric_limits<qint64>::max)(), created_utc_ms) ||
        !integer_in_range(object.value(QStringLiteral("logical_size_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), logical_size_bytes) ||
        !integer_in_range(object.value(QStringLiteral("stored_size_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), stored_size_bytes) ||
        !integer_in_range(object.value(QStringLiteral("deduplicated_block_count")), 0,
                          (std::numeric_limits<qint64>::max)(), deduplicated_block_count) ||
        !integer_in_range(object.value(QStringLiteral("deduplicated_logical_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), deduplicated_logical_bytes) ||
        !integer_in_range(object.value(QStringLiteral("source_count")), 0,
                          (std::numeric_limits<quint32>::max)(), source_count) ||
        !object.value(QStringLiteral("has_sidecar")).isBool()) {
        return false;
    }
    // Full has no parent; Incremental/Differential require a distinct parent UUID.
    if ((backup_type == 1) != !has_parent || (has_parent && parent_uuid == file_uuid)) {
        return false;
    }
    if (backup_type == 1 && chain_state != 1) {
        return false;
    }
    // file_set: Full or Incremental only; never Differential, never volume sidecar; no dedup metrics.
    if (content_kind == 2 &&
        (backup_type == 3 || object.value(QStringLiteral("has_sidecar")).toBool() ||
         deduplicated_block_count != 0 || deduplicated_logical_bytes != 0)) {
        return false;
    }
    if ((deduplicated_block_count == 0) != (deduplicated_logical_bytes == 0)) {
        return false;
    }
    result = {{QStringLiteral("fileUuid"), file_uuid},
              {QStringLiteral("backupSetUuid"), backup_set_uuid},
              {QStringLiteral("parentUuid"), parent_uuid},
              {QStringLiteral("backupType"), backup_type},
              {QStringLiteral("contentKind"), content_kind},
              {QStringLiteral("chainState"), chain_state},
              {QStringLiteral("createdUtcMs"), created_utc_ms},
              {QStringLiteral("logicalSizeBytes"), logical_size_bytes},
              {QStringLiteral("storedSizeBytes"), stored_size_bytes},
              {QStringLiteral("deduplicatedBlockCount"), deduplicated_block_count},
              {QStringLiteral("deduplicatedLogicalBytes"), deduplicated_logical_bytes},
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
    const QJsonObject payload{
        {QStringLiteral("minimum_api_version"), static_cast<qint64>(kServiceApiVersion)},
        {QStringLiteral("maximum_api_version"), static_cast<qint64>(kServiceApiVersion)}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), 1},
                   {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_recovery_point_request(const QString& request_id,
                                         const std::optional<QString>& continuation_token,
                                         const std::optional<QString>& repository_connection_id) {
    const QJsonObject page{
        {QStringLiteral("maximum_results"), static_cast<qint64>(kRecoveryPointPageSize)},
        {QStringLiteral("continuation_token"),
         continuation_token ? QJsonValue(*continuation_token) : QJsonValue(QJsonValue::Null)}};
    const QJsonObject payload{{QStringLiteral("repository_connection_id"),
                               repository_connection_id ? QJsonValue(*repository_connection_id)
                                                        : QJsonValue(QJsonValue::Null)},
                              {QStringLiteral("page"), page}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), 2},
                   {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encode_recovery_point_layout_request(const QString& request_id,
                                                const QString& repository_connection_id,
                                                const QString& recovery_point_id,
                                                const QString& archive_password) {
    const QJsonObject payload{{QStringLiteral("repository_connection_id"), repository_connection_id},
                              {QStringLiteral("recovery_point_id"), recovery_point_id},
                              {QStringLiteral("archive_password"), archive_password}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kGetRecoveryPointLayoutRequestKind},
                   {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                   {QStringLiteral("payload"), payload}})
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
    qint64 message_type = 0;
    return has_exact_keys(root, {"schema_version", "message_type", "request_id", "kind",
                                 "request_kind", "boundary_error_code", "message_code",
                                 "message_arguments", "payload"}) &&
           integer_in_range(root.value(QStringLiteral("schema_version")), kServiceSchemaVersion,
                            kServiceSchemaVersion, schema_version) &&
           integer_in_range(root.value(QStringLiteral("message_type")), 2, 2, message_type) &&
           root.value(QStringLiteral("request_id")).isString() &&
           root.value(QStringLiteral("request_id")).toString() == request_id &&
           root.value(QStringLiteral("message_code")).isString() &&
           valid_message_arguments(root.value(QStringLiteral("message_arguments"))) &&
           stable_code(root.value(QStringLiteral("message_code")).toString(), 128);
}

bool parse_service_info_response(const QJsonObject& root, ServiceInfo& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")), 1, 1, request_kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        root.value(QStringLiteral("message_code")).toString() != QStringLiteral("service.ready") ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const auto service = root.value(QStringLiteral("payload")).toObject();
    if (!has_exact_keys(service, {"minimum_api_version", "api_version", "state", "service_version",
                                  "capabilities"}) ||
        !service.value(QStringLiteral("service_version")).isString() ||
        !service.value(QStringLiteral("capabilities")).isArray()) {
        return false;
    }
    qint64 minimum_api_version = 0;
    qint64 api_version = 0;
    qint64 state = 0;
    if (!integer_in_range(service.value(QStringLiteral("minimum_api_version")), kServiceApiVersion,
                          kServiceApiVersion, minimum_api_version) ||
        !integer_in_range(service.value(QStringLiteral("api_version")), kServiceApiVersion,
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

namespace {

[[nodiscard]] bool parse_layout_partition(const QJsonObject& object, QVariantMap& result) {
    if (!has_exact_keys(object, {"partition_number", "offset_bytes", "size_bytes", "is_active",
                                 "mbr_type", "gpt_type_guid", "gpt_name", "volume_label",
                                 "filesystem"})) {
        return false;
    }
    qint64 partition_number = 0;
    qint64 offset_bytes = 0;
    qint64 size_bytes = 0;
    qint64 mbr_type = 0;
    if (!integer_in_range(object.value(QStringLiteral("partition_number")), 0, 1000,
                          partition_number) ||
        !integer_in_range(object.value(QStringLiteral("offset_bytes")), 0,
                          (std::numeric_limits<qint64>::max)(), offset_bytes) ||
        !integer_in_range(object.value(QStringLiteral("size_bytes")), 1,
                          (std::numeric_limits<qint64>::max)(), size_bytes) ||
        !integer_in_range(object.value(QStringLiteral("mbr_type")), 0, 255, mbr_type) ||
        !object.value(QStringLiteral("is_active")).isBool() ||
        !object.value(QStringLiteral("gpt_type_guid")).isString() ||
        !object.value(QStringLiteral("gpt_name")).isString() ||
        !object.value(QStringLiteral("volume_label")).isString() ||
        !object.value(QStringLiteral("filesystem")).isString()) {
        return false;
    }
    result = {{QStringLiteral("partitionNumber"), partition_number},
              {QStringLiteral("offsetBytes"), offset_bytes},
              {QStringLiteral("sizeBytes"), size_bytes},
              {QStringLiteral("isActive"), object.value(QStringLiteral("is_active")).toBool()},
              {QStringLiteral("mbrType"), mbr_type},
              {QStringLiteral("gptTypeGuid"),
               object.value(QStringLiteral("gpt_type_guid")).toString()},
              {QStringLiteral("gptName"), object.value(QStringLiteral("gpt_name")).toString()},
              {QStringLiteral("volumeLabel"),
               object.value(QStringLiteral("volume_label")).toString()},
              {QStringLiteral("filesystem"), object.value(QStringLiteral("filesystem")).toString()}};
    return true;
}

[[nodiscard]] bool parse_layout_disk(const QJsonObject& object, QVariantMap& result) {
    if (!has_exact_keys(object, {"disk_number", "disk_size_bytes", "partition_style", "model",
                                 "media_type", "partitions"}) ||
        !object.value(QStringLiteral("partitions")).isArray() ||
        !object.value(QStringLiteral("partition_style")).isString() ||
        !object.value(QStringLiteral("model")).isString() ||
        !object.value(QStringLiteral("media_type")).isString()) {
        return false;
    }
    qint64 disk_number = 0;
    qint64 disk_size = 0;
    if (!integer_in_range(object.value(QStringLiteral("disk_number")), 0, 1000, disk_number) ||
        !integer_in_range(object.value(QStringLiteral("disk_size_bytes")), 1,
                          (std::numeric_limits<qint64>::max)(), disk_size)) {
        return false;
    }
    const auto style = object.value(QStringLiteral("partition_style")).toString();
    if (style != QLatin1String("mbr") && style != QLatin1String("gpt") &&
        style != QLatin1String("raw")) {
        return false;
    }
    QVariantList partitions;
    for (const auto& value : object.value(QStringLiteral("partitions")).toArray()) {
        if (!value.isObject()) {
            return false;
        }
        QVariantMap partition;
        if (!parse_layout_partition(value.toObject(), partition)) {
            return false;
        }
        partitions.push_back(std::move(partition));
    }
    result = {{QStringLiteral("diskNumber"), disk_number},
              {QStringLiteral("diskSizeBytes"), disk_size},
              {QStringLiteral("partitionStyle"), style},
              {QStringLiteral("model"), object.value(QStringLiteral("model")).toString()},
              {QStringLiteral("mediaType"), object.value(QStringLiteral("media_type")).toString()},
              {QStringLiteral("partitions"), partitions}};
    return true;
}

[[nodiscard]] bool parse_layout_extent(const QJsonObject& object, QVariantMap& result) {
    if (!has_exact_keys(object, {"disk_number", "partition_number", "physical_offset",
                                 "volume_offset", "length"})) {
        return false;
    }
    qint64 disk_number = 0;
    qint64 partition_number = 0;
    qint64 physical_offset = 0;
    qint64 volume_offset = 0;
    qint64 length = 0;
    if (!integer_in_range(object.value(QStringLiteral("disk_number")), 0, 1000, disk_number) ||
        !integer_in_range(object.value(QStringLiteral("partition_number")), 0, 1000,
                          partition_number) ||
        !integer_in_range(object.value(QStringLiteral("physical_offset")), 0,
                          (std::numeric_limits<qint64>::max)(), physical_offset) ||
        !integer_in_range(object.value(QStringLiteral("volume_offset")), 0,
                          (std::numeric_limits<qint64>::max)(), volume_offset) ||
        !integer_in_range(object.value(QStringLiteral("length")), 1,
                          (std::numeric_limits<qint64>::max)(), length)) {
        return false;
    }
    result = {{QStringLiteral("diskNumber"), disk_number},
              {QStringLiteral("partitionNumber"), partition_number},
              {QStringLiteral("physicalOffset"), physical_offset},
              {QStringLiteral("volumeOffset"), volume_offset},
              {QStringLiteral("length"), length}};
    return true;
}

[[nodiscard]] bool parse_layout_volume(const QJsonObject& object, QVariantMap& result) {
    if (!has_exact_keys(object, {"volume_index", "letter", "label", "filesystem",
                                 "total_size_bytes", "extents"}) ||
        !object.value(QStringLiteral("extents")).isArray() ||
        !object.value(QStringLiteral("letter")).isString() ||
        !object.value(QStringLiteral("label")).isString() ||
        !object.value(QStringLiteral("filesystem")).isString()) {
        return false;
    }
    qint64 volume_index = 0;
    qint64 total_size = 0;
    if (!integer_in_range(object.value(QStringLiteral("volume_index")), 0, 1000, volume_index) ||
        !integer_in_range(object.value(QStringLiteral("total_size_bytes")), 1,
                          (std::numeric_limits<qint64>::max)(), total_size)) {
        return false;
    }
    QVariantList extents;
    for (const auto& value : object.value(QStringLiteral("extents")).toArray()) {
        if (!value.isObject()) {
            return false;
        }
        QVariantMap extent;
        if (!parse_layout_extent(value.toObject(), extent)) {
            return false;
        }
        extents.push_back(std::move(extent));
    }
    if (extents.isEmpty()) {
        return false;
    }
    result = {{QStringLiteral("volumeIndex"), volume_index},
              {QStringLiteral("letter"), object.value(QStringLiteral("letter")).toString()},
              {QStringLiteral("label"), object.value(QStringLiteral("label")).toString()},
              {QStringLiteral("filesystem"), object.value(QStringLiteral("filesystem")).toString()},
              {QStringLiteral("totalSizeBytes"), total_size},
              {QStringLiteral("extents"), extents}};
    return true;
}

} // namespace

bool parse_delete_plan_response(const QJsonObject& root, QVariantMap& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")),
                          kPlanDeleteRecoveryPointsRequestKind,
                          kPlanDeleteRecoveryPointsRequestKind, request_kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const auto payload = root.value(QStringLiteral("payload")).toObject();
    if (!has_exact_keys(payload, {"plan_token", "operation_id", "repository_connection_id",
                                  "root_recovery_point_id", "targets", "expires_utc_ms"})) {
        return false;
    }
    const auto plan_token = payload.value(QStringLiteral("plan_token")).toString();
    const auto operation_id = payload.value(QStringLiteral("operation_id")).toString();
    const auto connection_id =
        payload.value(QStringLiteral("repository_connection_id")).toString();
    const auto root_rp = payload.value(QStringLiteral("root_recovery_point_id")).toString();
    qint64 expires_utc_ms = 0;
    if (!payload.value(QStringLiteral("plan_token")).isString() || !stable_code(plan_token, 128) ||
        !payload.value(QStringLiteral("operation_id")).isString() ||
        !stable_code(operation_id, 128) ||
        !payload.value(QStringLiteral("repository_connection_id")).isString() ||
        !stable_code(connection_id, 128) ||
        !payload.value(QStringLiteral("root_recovery_point_id")).isString() ||
        !canonical_uuid(root_rp) || !payload.value(QStringLiteral("targets")).isArray() ||
        !integer_in_range(payload.value(QStringLiteral("expires_utc_ms")), 0,
                          (std::numeric_limits<qint64>::max)(), expires_utc_ms)) {
        return false;
    }
    QVariantList targets;
    QSet<QString> seen_ids;
    for (const auto& value : payload.value(QStringLiteral("targets")).toArray()) {
        if (!value.isObject()) {
            return false;
        }
        const auto target = value.toObject();
        if (!has_exact_keys(target,
                            {"recovery_point_id", "catalog_generation", "member_count"})) {
            return false;
        }
        const auto rp_id = target.value(QStringLiteral("recovery_point_id")).toString();
        qint64 generation = 0;
        qint64 member_count = 0;
        if (!target.value(QStringLiteral("recovery_point_id")).isString() ||
            !canonical_uuid(rp_id) || seen_ids.contains(rp_id) ||
            !integer_in_range(target.value(QStringLiteral("catalog_generation")), 0,
                              (std::numeric_limits<qint64>::max)(), generation) ||
            !integer_in_range(target.value(QStringLiteral("member_count")), 0,
                              (std::numeric_limits<quint32>::max)(), member_count)) {
            return false;
        }
        seen_ids.insert(rp_id);
        targets.push_back(QVariantMap{{QStringLiteral("recoveryPointId"), rp_id},
                                      {QStringLiteral("catalogGeneration"), generation},
                                      {QStringLiteral("memberCount"), member_count}});
    }
    result = {{QStringLiteral("planToken"), plan_token},
              {QStringLiteral("operationId"), operation_id},
              {QStringLiteral("repositoryConnectionId"), connection_id},
              {QStringLiteral("rootRecoveryPointId"), root_rp},
              {QStringLiteral("targetCount"), static_cast<qint64>(targets.size())},
              {QStringLiteral("targets"), targets},
              {QStringLiteral("expiresUtcMs"), expires_utc_ms}};
    return true;
}

bool parse_recovery_point_layout_response(const QJsonObject& root, QVariantMap& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")),
                          kGetRecoveryPointLayoutRequestKind, kGetRecoveryPointLayoutRequestKind,
                          request_kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        root.value(QStringLiteral("message_code")).toString() !=
            QStringLiteral("recovery_point.layout_ready") ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const auto payload = root.value(QStringLiteral("payload")).toObject();
    if (!has_exact_keys(payload,
                        {"repository_connection_id", "recovery_point_id", "disks", "volumes"}) ||
        !payload.value(QStringLiteral("repository_connection_id")).isString() ||
        !payload.value(QStringLiteral("recovery_point_id")).isString() ||
        !payload.value(QStringLiteral("disks")).isArray() ||
        !payload.value(QStringLiteral("volumes")).isArray()) {
        return false;
    }
    QVariantList disks;
    for (const auto& value : payload.value(QStringLiteral("disks")).toArray()) {
        if (!value.isObject()) {
            return false;
        }
        QVariantMap disk;
        if (!parse_layout_disk(value.toObject(), disk)) {
            return false;
        }
        disks.push_back(std::move(disk));
    }
    QVariantList volumes;
    for (const auto& value : payload.value(QStringLiteral("volumes")).toArray()) {
        if (!value.isObject()) {
            return false;
        }
        QVariantMap volume;
        if (!parse_layout_volume(value.toObject(), volume)) {
            return false;
        }
        volumes.push_back(std::move(volume));
    }
    if (disks.isEmpty() || volumes.isEmpty()) {
        return false;
    }
    result = {{QStringLiteral("repositoryConnectionId"),
               payload.value(QStringLiteral("repository_connection_id")).toString()},
              {QStringLiteral("recoveryPointId"),
               payload.value(QStringLiteral("recovery_point_id")).toString()},
              {QStringLiteral("disks"), disks},
              {QStringLiteral("volumes"), volumes}};
    return true;
}

bool parse_recovery_point_response(const QJsonObject& root, RecoveryPointPage& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")), 2, 2, request_kind) ||
        !integer_in_range(root.value(QStringLiteral("boundary_error_code")), 0, 0, error) ||
        !root.value(QStringLiteral("payload")).isObject()) {
        return false;
    }
    const auto payload = root.value(QStringLiteral("payload")).toObject();
    if (!has_exact_keys(payload, {"repository_connection_id", "catalog"}) ||
        !payload.value(QStringLiteral("catalog")).isObject()) {
        return false;
    }
    const auto connection_value = payload.value(QStringLiteral("repository_connection_id"));
    if (connection_value.isNull()) {
        result.repository_connection_id.reset();
    } else if (connection_value.isString() &&
               stable_code(connection_value.toString(), kMaximumStableCodeCharacters)) {
        result.repository_connection_id = connection_value.toString();
    } else {
        return false;
    }
    const auto page = payload.value(QStringLiteral("catalog")).toObject();
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
           message == QStringLiteral("repository.catalog_ready");
}

bool is_repository_failure_response(const QJsonObject& root) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    return integer_in_range(root.value(QStringLiteral("kind")), 3, 3, kind) &&
           integer_in_range(root.value(QStringLiteral("request_kind")), 2, 2, request_kind) &&
           integer_in_range(root.value(QStringLiteral("boundary_error_code")), 1, 11, error) &&
           root.value(QStringLiteral("message_code")).toString() ==
               QStringLiteral("repository.query_failed") &&
           root.value(QStringLiteral("payload")).isNull();
}

bool is_recovery_point_layout_failure_response(const QJsonObject& root) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    return integer_in_range(root.value(QStringLiteral("kind")), 3, 3, kind) &&
           integer_in_range(root.value(QStringLiteral("request_kind")),
                            kGetRecoveryPointLayoutRequestKind, kGetRecoveryPointLayoutRequestKind,
                            request_kind) &&
           integer_in_range(root.value(QStringLiteral("boundary_error_code")), 1, 11, error) &&
           root.value(QStringLiteral("message_code")).toString() ==
               QStringLiteral("recovery_point.layout_failed") &&
           root.value(QStringLiteral("payload")).isNull();
}

namespace {

[[nodiscard]] bool parse_optional_int64(const QJsonValue& value, std::optional<qint64>& result) {
    if (value.isNull()) {
        result.reset();
        return true;
    }
    qint64 integer = 0;
    if (!integer_in_range(value, 0, (std::numeric_limits<qint64>::max)(), integer)) {
        return false;
    }
    result = integer;
    return true;
}

[[nodiscard]] bool parse_job_item_object(const QJsonObject& object, QVariantMap& result) {
    if (!has_exact_keys(object,
                        {"job_id", "trace_id", "operation", "state", "content_kind", "created_utc_ms",
                         "started_utc_ms", "completed_utc_ms", "progress", "message_code",
                         "source_ids", "schedule_id", "repository_connection_id",
                         "requested_backup_type", "effective_backup_type", "effective_parent_uuid",
                         "incremental_downgrade_reason"})) {
        return false;
    }
    qint64 operation = 0;
    qint64 state = 0;
    std::optional<qint64> content_kind;
    qint64 created_utc_ms = 0;
    std::optional<qint64> started_utc_ms;
    std::optional<qint64> completed_utc_ms;
    if (!object.value(QStringLiteral("job_id")).isString() ||
        !stable_code(object.value(QStringLiteral("job_id")).toString(), 128) ||
        !object.value(QStringLiteral("trace_id")).isString() ||
        !stable_code(object.value(QStringLiteral("trace_id")).toString(), 128) ||
        !integer_in_range(object.value(QStringLiteral("operation")), 1, 4, operation) ||
        !integer_in_range(object.value(QStringLiteral("state")), 1, 7, state) ||
        !parse_optional_int64(object.value(QStringLiteral("content_kind")), content_kind) ||
        (content_kind && (*content_kind < 1 || *content_kind > 2)) ||
        !integer_in_range(object.value(QStringLiteral("created_utc_ms")), 0,
                          (std::numeric_limits<qint64>::max)(), created_utc_ms) ||
        !parse_optional_int64(object.value(QStringLiteral("started_utc_ms")), started_utc_ms) ||
        !parse_optional_int64(object.value(QStringLiteral("completed_utc_ms")), completed_utc_ms) ||
        !object.value(QStringLiteral("message_code")).isString() ||
        !stable_code(object.value(QStringLiteral("message_code")).toString(), 128)) {
        return false;
    }
    QVariantList source_ids;
    QSet<QString> seen_source_ids;
    QString connection_id;
    QString schedule_id;
    const auto source_value = object.value(QStringLiteral("source_ids"));
    if (!source_value.isArray() || source_value.toArray().size() > 100) {
        return false;
    }
    for (const auto& value : source_value.toArray()) {
        const auto source_id = value.toString();
        if (!value.isString() || !stable_code(source_id, 128) ||
            seen_source_ids.contains(source_id)) {
            return false;
        }
        seen_source_ids.insert(source_id);
        source_ids.push_back(source_id);
    }
    const auto schedule_value = object.value(QStringLiteral("schedule_id"));
    if (!schedule_value.isNull()) {
        if (!schedule_value.isString() || !stable_code(schedule_value.toString(), 128)) {
            return false;
        }
        schedule_id = schedule_value.toString();
    }
    if (operation == 1 && schedule_id.isEmpty()) {
        return false; // backup jobs must own a schedule
    }
    if (operation != 1 && !schedule_id.isEmpty()) {
        return false;
    }
    const auto connection_value = object.value(QStringLiteral("repository_connection_id"));
    if (!connection_value.isNull()) {
        if (!connection_value.isString() || !stable_code(connection_value.toString(), 128)) {
            return false;
        }
        connection_id = connection_value.toString();
    }
    std::optional<qint64> requested_backup_type;
    std::optional<qint64> effective_backup_type;
    std::optional<qint64> incremental_downgrade_reason;
    QString effective_parent_uuid;
    bool has_effective_parent = false;
    if (!parse_optional_int64(object.value(QStringLiteral("requested_backup_type")),
                              requested_backup_type) ||
        (requested_backup_type &&
         (*requested_backup_type < 1 || *requested_backup_type > 3)) ||
        !parse_optional_int64(object.value(QStringLiteral("effective_backup_type")),
                              effective_backup_type) ||
        (effective_backup_type &&
         (*effective_backup_type < 1 || *effective_backup_type > 3)) ||
        !optional_uuid(object.value(QStringLiteral("effective_parent_uuid")), effective_parent_uuid,
                       has_effective_parent) ||
        !parse_optional_int64(object.value(QStringLiteral("incremental_downgrade_reason")),
                              incremental_downgrade_reason) ||
        (incremental_downgrade_reason &&
         *incremental_downgrade_reason != 1 && *incremental_downgrade_reason != 2 &&
         *incremental_downgrade_reason != 3 && *incremental_downgrade_reason != 9)) {
        return false;
    }
    QVariantMap map{
        {QStringLiteral("jobId"), object.value(QStringLiteral("job_id")).toString()},
        {QStringLiteral("traceId"), object.value(QStringLiteral("trace_id")).toString()},
        {QStringLiteral("operation"), operation},
        {QStringLiteral("state"), state},
        {QStringLiteral("createdUtcMs"), created_utc_ms},
        {QStringLiteral("messageCode"), object.value(QStringLiteral("message_code")).toString()},
        {QStringLiteral("sourceIds"), source_ids},
        {QStringLiteral("scheduleId"), schedule_id},
        {QStringLiteral("connectionId"), connection_id}};
    if (content_kind) {
        map.insert(QStringLiteral("contentKind"), *content_kind);
    }
    if (started_utc_ms) {
        map.insert(QStringLiteral("startedUtcMs"), *started_utc_ms);
    }
    if (completed_utc_ms) {
        map.insert(QStringLiteral("completedUtcMs"), *completed_utc_ms);
    }
    if (requested_backup_type) {
        map.insert(QStringLiteral("requestedBackupType"), *requested_backup_type);
    }
    if (effective_backup_type) {
        map.insert(QStringLiteral("effectiveBackupType"), *effective_backup_type);
    }
    if (has_effective_parent) {
        map.insert(QStringLiteral("effectiveParentUuid"), effective_parent_uuid);
    }
    if (incremental_downgrade_reason) {
        map.insert(QStringLiteral("incrementalDowngradeReason"), *incremental_downgrade_reason);
    }
    const auto progress_value = object.value(QStringLiteral("progress"));
    if (!progress_value.isNull()) {
        if (!progress_value.isObject()) {
            return false;
        }
        const auto progress = progress_value.toObject();
        if (!has_exact_keys(progress,
                            {"schema_version", "job_id", "trace_id", "phase", "logical_bytes",
                             "processed_bytes", "stored_bytes", "discovered_entries",
                             "processed_entries", "message_code"})) {
            return false;
        }
        const auto outer_job_id = object.value(QStringLiteral("job_id")).toString();
        const auto outer_trace_id = object.value(QStringLiteral("trace_id")).toString();
        const auto progress_job_id = progress.value(QStringLiteral("job_id")).toString();
        const auto progress_trace_id = progress.value(QStringLiteral("trace_id")).toString();
        const auto progress_message = progress.value(QStringLiteral("message_code")).toString();
        qint64 schema_version = 0;
        qint64 phase = 0;
        std::optional<qint64> logical_bytes;
        qint64 processed_bytes = 0;
        qint64 stored_bytes = 0;
        qint64 discovered_entries = 0;
        qint64 processed_entries = 0;
        if (!integer_in_range(progress.value(QStringLiteral("schema_version")),
                              static_cast<qint64>(kServiceSchemaVersion),
                              static_cast<qint64>(kServiceSchemaVersion), schema_version) ||
            progress_job_id != outer_job_id || progress_trace_id != outer_trace_id ||
            !stable_code(progress_message, 128) ||
            !integer_in_range(progress.value(QStringLiteral("phase")), 0, 6, phase) ||
            !parse_optional_int64(progress.value(QStringLiteral("logical_bytes")), logical_bytes) ||
            !integer_in_range(progress.value(QStringLiteral("processed_bytes")), 0,
                              (std::numeric_limits<qint64>::max)(), processed_bytes) ||
            !integer_in_range(progress.value(QStringLiteral("stored_bytes")), 0,
                              (std::numeric_limits<qint64>::max)(), stored_bytes) ||
            !integer_in_range(progress.value(QStringLiteral("discovered_entries")), 0,
                              (std::numeric_limits<qint64>::max)(), discovered_entries) ||
            !integer_in_range(progress.value(QStringLiteral("processed_entries")), 0,
                              (std::numeric_limits<qint64>::max)(), processed_entries) ||
            (logical_bytes && processed_bytes > *logical_bytes)) {
            return false;
        }
        map.insert(QStringLiteral("progressPhase"), phase);
        if (logical_bytes) {
            map.insert(QStringLiteral("progressLogicalBytes"), *logical_bytes);
        }
        map.insert(QStringLiteral("progressProcessedBytes"), processed_bytes);
        map.insert(QStringLiteral("progressStoredBytes"), stored_bytes);
        map.insert(QStringLiteral("progressDiscoveredEntries"), discovered_entries);
        map.insert(QStringLiteral("progressProcessedEntries"), processed_entries);
    }
    result = std::move(map);
    return true;
}

[[nodiscard]] bool parse_job_item(const QJsonValue& value, QVariantMap& result) {
    if (!value.isObject()) {
        return false;
    }
    return parse_job_item_object(value.toObject(), result);
}

} // namespace

bool parse_job_summary_object(const QJsonObject& object, QVariantMap& result) {
    return parse_job_item_object(object, result);
}

QByteArray encode_job_list_request(const QString& request_id,
                                   const std::optional<QString>& continuation_token) {
    const QJsonObject page{
        {QStringLiteral("maximum_results"), static_cast<qint64>(kJobPageSize)},
        {QStringLiteral("continuation_token"),
         continuation_token ? QJsonValue(*continuation_token) : QJsonValue(QJsonValue::Null)}};
    const QJsonObject payload{{QStringLiteral("page"), page},
                              {QStringLiteral("operation"), QJsonValue(QJsonValue::Null)},
                              {QStringLiteral("state"), QJsonValue(QJsonValue::Null)}};
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), static_cast<qint64>(kServiceSchemaVersion)},
                   {QStringLiteral("message_type"), 1},
                   {QStringLiteral("request_id"), request_id},
                   {QStringLiteral("kind"), kListJobsRequestKind},
                   {QStringLiteral("idempotency_key"), QJsonValue(QJsonValue::Null)},
                   {QStringLiteral("payload"), payload}})
        .toJson(QJsonDocument::Compact);
}

bool parse_job_list_response(const QJsonObject& root, JobPage& result) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    if (!integer_in_range(root.value(QStringLiteral("kind")), 1, 1, kind) ||
        !integer_in_range(root.value(QStringLiteral("request_kind")), kListJobsRequestKind,
                          kListJobsRequestKind, request_kind) ||
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
    if (items.size() > static_cast<qsizetype>(kJobPageSize)) {
        return false;
    }
    QVariantList parsed_items;
    QString previous_id;
    for (const auto& item : items) {
        QVariantMap parsed;
        if (!parse_job_item(item, parsed)) {
            return false;
        }
        const auto job_id = parsed.value(QStringLiteral("jobId")).toString();
        if (!previous_id.isEmpty() && job_id == previous_id) {
            return false;
        }
        previous_id = job_id;
        parsed_items.push_back(std::move(parsed));
    }
    if (!parse_token(payload.value(QStringLiteral("continuation_token")),
                     result.continuation_token)) {
        return false;
    }
    result.items = std::move(parsed_items);
    return true;
}

bool is_job_failure_response(const QJsonObject& root) {
    qint64 kind = 0;
    qint64 request_kind = 0;
    qint64 error = 0;
    return integer_in_range(root.value(QStringLiteral("kind")), kRequestFailedResponseKind,
                            kRequestFailedResponseKind, kind) &&
           integer_in_range(root.value(QStringLiteral("request_kind")), kListJobsRequestKind,
                            kListJobsRequestKind, request_kind) &&
           integer_in_range(root.value(QStringLiteral("boundary_error_code")), 1, 11, error) &&
           root.value(QStringLiteral("payload")).isNull();
}

} // namespace aegra::desktop
