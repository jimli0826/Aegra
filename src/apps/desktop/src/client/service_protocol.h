#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

namespace aegra::desktop {

inline constexpr quint32 kServiceSchemaVersion = 3;
inline constexpr quint32 kServiceApiVersion = 3;
inline constexpr quint32 kRecoveryPointPageSize = 100;
inline constexpr quint32 kJobPageSize = 100;
inline constexpr quint32 kInventoryPageSize = 100;
inline constexpr quint32 kConnectionPageSize = 100;
inline constexpr quint32 kSchedulePageSize = 100;
inline constexpr int kListRepositoryConnectionsRequestKind = 3;
inline constexpr int kListSourceInventoryRequestKind = 4;
inline constexpr int kListJobsRequestKind = 5;
inline constexpr int kListSchedulesRequestKind = 6;
inline constexpr int kGetRecoveryPointLayoutRequestKind = 12;
inline constexpr int kAddRepositoryConnectionRequestKind = 32;
inline constexpr int kImportRepositoryConnectionRequestKind = 33;
inline constexpr int kTestRepositoryConnectionRequestKind = 34;
inline constexpr int kSetDefaultRepositoryRequestKind = 35;
inline constexpr int kRemoveRepositoryConnectionRequestKind = 36;
inline constexpr int kPrepareRestoreRequestKind = 9;
inline constexpr int kStartBackupRequestKind = 37;
inline constexpr int kCancelJobRequestKind = 38;
inline constexpr int kStartRestoreRequestKind = 40;
inline constexpr int kUpsertScheduleRequestKind = 43;
inline constexpr int kDeleteScheduleRequestKind = 44;
inline constexpr int kScheduleTriggerDaily = 1;
inline constexpr int kScheduleTriggerWeekly = 2;
inline constexpr int kCommandAcceptedResponseKind = 2;
inline constexpr int kRequestFailedResponseKind = 3;
inline constexpr int kBackupTypeFull = 1;
inline constexpr int kBackupTypeIncremental = 2;
inline constexpr int kCommandDispositionAccepted = 1;
inline constexpr int kCommandDispositionReplayed = 2;

struct ServiceInfo final {
    QString version;
    QStringList capabilities;
};

struct RecoveryPointPage final {
    bool configured{false};
    std::optional<QString> repository_connection_id;
    QString repository_uuid;
    QVariantList items;
    std::optional<QString> continuation_token;
};

struct JobPage final {
    QVariantList items;
    std::optional<QString> continuation_token;
};

struct SourceInventoryPage final {
    QVariantList items;
    std::optional<QString> continuation_token;
};

struct RepositoryConnectionPage final {
    QVariantList items;
    std::optional<QString> continuation_token;
};

struct SchedulePage final {
    QVariantList items;
    std::optional<QString> continuation_token;
};

struct CommandAck final {
    QString command_id;
    qint64 disposition{0};
    QString resource_id;
    bool has_resource_id{false};
    QString message_code;
};

[[nodiscard]] QByteArray encode_service_info_request(const QString& request_id);
[[nodiscard]] QByteArray encode_recovery_point_request(
    const QString& request_id, const std::optional<QString>& continuation_token,
    const std::optional<QString>& repository_connection_id = std::nullopt);
[[nodiscard]] QByteArray encode_recovery_point_layout_request(const QString& request_id,
                                                              const QString& repository_connection_id,
                                                              const QString& recovery_point_id,
                                                              const QString& archive_password = {});
[[nodiscard]] QByteArray encode_job_list_request(const QString& request_id,
                                                 const std::optional<QString>& continuation_token);
[[nodiscard]] QByteArray
encode_source_inventory_request(const QString& request_id,
                                const std::optional<QString>& continuation_token,
                                bool include_unavailable = true);
[[nodiscard]] QByteArray
encode_repository_connection_list_request(const QString& request_id,
                                          const std::optional<QString>& continuation_token);
[[nodiscard]] QByteArray
encode_schedule_list_request(const QString& request_id,
                             const std::optional<QString>& continuation_token);
[[nodiscard]] QByteArray encode_start_backup_request(const QString& request_id,
                                                     const QString& idempotency_key,
                                                     const QString& schedule_id,
                                                     int backup_type = kBackupTypeFull);
[[nodiscard]] QByteArray encode_prepare_restore_request(const QString& request_id,
                                                        const QString& connection_id,
                                                        const QString& recovery_point_id,
                                                        const QString& target_source_id,
                                                        int source_disk_number,
                                                        const QString& archive_password = {});
[[nodiscard]] QByteArray encode_start_restore_request(const QString& request_id,
                                                      const QString& idempotency_key,
                                                      const QString& preflight_token,
                                                      const QString& archive_password = {},
                                                      bool preserve_disk_signature = true,
                                                      bool auto_expand_last_partition = true);
[[nodiscard]] QByteArray encode_cancel_job_request(const QString& request_id,
                                                   const QString& idempotency_key,
                                                   const QString& job_id);
[[nodiscard]] QByteArray encode_repository_connection_input_request(const QString& request_id,
                                                                    const QString& idempotency_key,
                                                                    int request_kind,
                                                                    const QString& display_name,
                                                                    const QString& locator);
[[nodiscard]] QByteArray
encode_repository_connection_resource_request(const QString& request_id,
                                              const QString& idempotency_key, int request_kind,
                                              const QString& connection_id);
[[nodiscard]] QByteArray encode_upsert_schedule_request(
    const QString& request_id, const QString& idempotency_key, const QString& schedule_id,
    const QString& display_name, bool enabled, const QVariantList& source_ids,
    const QString& repository_connection_id, int backup_type, int trigger_kind,
    int local_minute_of_day, int weekday_mask, const QString& timezone_id,
    bool exclude_page_and_hibernation_files = true, bool encryption_enabled = false,
    const QString& archive_password = {});
[[nodiscard]] QByteArray encode_delete_schedule_request(const QString& request_id,
                                                        const QString& idempotency_key,
                                                        const QString& schedule_id);

[[nodiscard]] bool parse_response_root(const QByteArray& body, const QString& request_id,
                                       QJsonObject& root);
[[nodiscard]] bool parse_service_info_response(const QJsonObject& root, ServiceInfo& result);
[[nodiscard]] bool parse_recovery_point_response(const QJsonObject& root,
                                                 RecoveryPointPage& result);
/// On success, result is { repositoryConnectionId, recoveryPointId, volumes: [...] }.
[[nodiscard]] bool parse_recovery_point_layout_response(const QJsonObject& root,
                                                        QVariantMap& result);
[[nodiscard]] bool parse_job_list_response(const QJsonObject& root, JobPage& result);
[[nodiscard]] bool parse_source_inventory_response(const QJsonObject& root,
                                                   SourceInventoryPage& result);
[[nodiscard]] bool parse_repository_connection_list_response(const QJsonObject& root,
                                                             RepositoryConnectionPage& result);
[[nodiscard]] bool parse_schedule_list_response(const QJsonObject& root, SchedulePage& result);
[[nodiscard]] bool parse_command_ack_response(const QJsonObject& root, int expected_request_kind,
                                              CommandAck& result);
// Parses one JobSummary JSON object into the desktop map form used by JobModel.
[[nodiscard]] bool parse_job_summary_object(const QJsonObject& object, QVariantMap& result);
[[nodiscard]] bool is_repository_failure_response(const QJsonObject& root);
[[nodiscard]] bool is_recovery_point_layout_failure_response(const QJsonObject& root);
[[nodiscard]] bool is_job_failure_response(const QJsonObject& root);
[[nodiscard]] bool is_inventory_failure_response(const QJsonObject& root);
[[nodiscard]] bool is_connection_list_failure_response(const QJsonObject& root);
[[nodiscard]] bool is_schedule_list_failure_response(const QJsonObject& root);
[[nodiscard]] bool is_command_failure_response(const QJsonObject& root, int expected_request_kind);

} // namespace aegra::desktop
