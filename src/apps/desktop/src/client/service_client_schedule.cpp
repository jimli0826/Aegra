#include "client/service_client.h"

#include "client/service_protocol.h"
#include "locale/message_code_map.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <QTimeZone>
#include <QUuid>
#include <QVariantMap>

#include <algorithm>

namespace aegra::desktop {
namespace {

constexpr int kMaximumSchedules = 500;

[[nodiscard]] QList<int> parse_times_of_day_minutes(const QString& time_of_day) {
    QList<int> minutes;
    const auto chunks =
        time_of_day.split(QRegularExpression(QStringLiteral("[,;]")), Qt::SkipEmptyParts);
    for (const auto& chunk : chunks) {
        const auto parts = chunk.trimmed().split(QLatin1Char(':'));
        int hour = 2;
        int minute = 0;
        if (!parts.isEmpty()) {
            bool ok = false;
            hour = parts[0].toInt(&ok);
            if (!ok) {
                hour = 2;
            }
        }
        if (parts.size() > 1) {
            bool ok = false;
            minute = parts[1].toInt(&ok);
            if (!ok) {
                minute = 0;
            }
        }
        hour = qBound(0, hour, 23);
        minute = qBound(0, minute, 59);
        const auto value = hour * 60 + minute;
        if (!minutes.contains(value)) {
            minutes.push_back(value);
        }
        if (minutes.size() >= 8) {
            break;
        }
    }
    if (minutes.isEmpty()) {
        minutes.push_back(2 * 60);
    }
    std::sort(minutes.begin(), minutes.end());
    return minutes;
}

[[nodiscard]] QString format_next_run(const QVariant& next_run_utc_ms) {
    if (!next_run_utc_ms.isValid() || next_run_utc_ms.isNull()) {
        return {};
    }
    bool ok = false;
    const auto ms = next_run_utc_ms.toLongLong(&ok);
    if (!ok || ms <= 0) {
        return {};
    }
    const auto dt = QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toLocalTime();
    return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace

QVariantList ServiceClient::schedules() const { return schedules_; }

ScheduleListModel* ServiceClient::scheduleList() noexcept { return &schedule_list_; }

void ServiceClient::publish_schedules(QVariantList items) {
    schedules_ = std::move(items);
    schedule_list_.set_items(schedules_);
    emit schedulesChanged();
}

QVariantList ServiceClient::sourceIdsForSchedule(const QString& schedule_id) const {
    if (schedule_id.isEmpty()) {
        return {};
    }
    for (const auto& item : schedules_) {
        const auto map = item.toMap();
        const auto id = map.value(QStringLiteral("scheduleId")).toString();
        if (id == schedule_id || map.value(QStringLiteral("id")).toString() == schedule_id) {
            return map.value(QStringLiteral("sourceIds")).toList();
        }
    }
    return {};
}

QVariantList ServiceClient::displayChainsForSchedule(const QString& schedule_id) const {
    if (schedule_id.isEmpty()) {
        return {};
    }
    for (const auto& item : schedules_) {
        const auto map = item.toMap();
        const auto id = map.value(QStringLiteral("scheduleId")).toString();
        if (id != schedule_id && map.value(QStringLiteral("id")).toString() != schedule_id) {
            continue;
        }
        QVariantList chains;
        for (const auto& summary : map.value(QStringLiteral("selectionSummaries")).toList()) {
            const auto summary_map = summary.toMap();
            const auto label = summary_map.value(QStringLiteral("displayLabel")).toString();
            // Special-folder product roots (Desktop/Downloads/…) rehydrate by short label even
            // when older summaries stored the volume-relative path as display_chain.
            QStringList chain;
            if (label == QLatin1String("Desktop") || label == QLatin1String("Downloads") ||
                label == QLatin1String("Documents") || label == QLatin1String("Pictures") ||
                label == QLatin1String("Music") || label == QLatin1String("Videos")) {
                chain.push_back(label);
            } else {
                chain = summary_map.value(QStringLiteral("displayChain")).toStringList();
                if (chain.isEmpty() && !label.isEmpty()) {
                    chain.push_back(label);
                }
            }
            if (!chain.isEmpty()) {
                chains.push_back(chain);
            }
        }
        return chains;
    }
    return {};
}

void ServiceClient::logScheduleEdit(const QString& message) {
    QDir().mkpath(QStringLiteral("D:/Work/OpenSource/Aegra/out"));
    QFile file(QStringLiteral("D:/Work/OpenSource/Aegra/out/schedule-edit-debug.log"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << " " << message << "\n";
}

bool ServiceClient::schedulesLoading() const noexcept { return schedules_loading_; }

bool ServiceClient::scheduleCommandBusy() const noexcept { return schedule_command_busy_; }

bool ServiceClient::scheduleCommandBlocksUi() const noexcept {
    // Row enable/disable is optimistic and must not flash LoadingOverlay.
    return schedule_command_busy_ && !schedule_enable_patch_active_;
}

void ServiceClient::set_schedule_command_busy(const bool busy) {
    if (schedule_command_busy_ == busy) {
        return;
    }
    schedule_command_busy_ = busy;
    emit scheduleCommandChanged();
    // Skip global loading churn for quiet enable/disable patches.
    if (!schedule_enable_patch_active_) {
        emit loadingChanged();
    }
}

bool ServiceClient::schedulesAvailable() const noexcept { return schedules_available_; }

QString ServiceClient::schedulesErrorText() const {
    return schedules_error_code_.isEmpty() ? QString{}
                                           : localize_message_code(schedules_error_code_);
}

void ServiceClient::refreshSchedules() {
    if (state_ != State::kReady || !schedules_available_ || schedules_loading_ ||
        (!schedule_request_id_.isEmpty() &&
         coordinator_->has_pending_request(schedule_request_id_))) {
        return;
    }
    start_schedule_query();
}

void ServiceClient::enrich_schedules_with_connections() {
    for (auto& item : schedules_) {
        auto map = item.toMap();
        const auto connection_id = map.value(QStringLiteral("connectionId")).toString();
        if (const auto found = connections_.find(connection_id)) {
            map.insert(QStringLiteral("destinationName"), found->display_name);
            map.insert(QStringLiteral("destinationPath"), found->display_name);
        }
        map.insert(QStringLiteral("nextRun"),
                   format_next_run(map.value(QStringLiteral("nextRunUtcMs"))));
        item = map;
    }
}

bool ServiceClient::patch_schedule_enabled(const QString& schedule_id, const bool enabled) {
    if (schedule_id.isEmpty()) {
        return false;
    }
    // Keep QVariantList cache in sync for C++ helpers; model uses row-level dataChanged only.
    for (auto& item : schedules_) {
        auto map = item.toMap();
        if (map.value(QStringLiteral("scheduleId")).toString() != schedule_id &&
            map.value(QStringLiteral("id")).toString() != schedule_id) {
            continue;
        }
        map.insert(QStringLiteral("enabled"), enabled);
        item = map;
        break;
    }
    return schedule_list_.set_enabled(schedule_id, enabled);
}

void ServiceClient::start_schedule_query() {
    if (!schedules_available_) {
        return;
    }
    schedules_error_code_.clear();
    schedules_loading_ = true;
    pending_schedules_.clear();
    schedule_requested_token_.reset();
    emit schedulesChanged();
    emit loadingChanged();

    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    schedule_request_id_ = request_id;
    const auto body = encode_schedule_list_request(request_id, std::nullopt);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_schedule_list_frame(frame_body);
        });
    if (!started) {
        finish_schedule_failure(QStringLiteral("schedule.query_failed"));
    }
}

RequestDisposition ServiceClient::handle_schedule_list_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_schedule_list_failure_response(root)) {
        finish_schedule_failure(QStringLiteral("schedule.query_failed"));
        return RequestDisposition::kFinished;
    }
    SchedulePage page;
    if (!parse_schedule_list_response(root, page)) {
        // Soft-fail the schedule domain; do not escalate to a transport protocol drop.
        finish_schedule_failure(QStringLiteral("schedule.query_failed"));
        return RequestDisposition::kFinished;
    }
    if ((page.continuation_token && page.continuation_token == schedule_requested_token_) ||
        pending_schedules_.size() + page.items.size() > kMaximumSchedules) {
        finish_schedule_failure(QStringLiteral("schedule.query_failed"));
        return RequestDisposition::kFinished;
    }
    QSet<QString> seen_ids;
    for (const auto& existing : pending_schedules_) {
        seen_ids.insert(existing.toMap().value(QStringLiteral("scheduleId")).toString());
    }
    for (auto& item : page.items) {
        const auto schedule_id = item.toMap().value(QStringLiteral("scheduleId")).toString();
        if (seen_ids.contains(schedule_id)) {
            finish_schedule_failure(QStringLiteral("schedule.query_failed"));
            return RequestDisposition::kFinished;
        }
        seen_ids.insert(schedule_id);
        pending_schedules_.push_back(std::move(item));
    }
    if (page.continuation_token) {
        schedule_requested_token_ = page.continuation_token;
        const auto next_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto next_body = encode_schedule_list_request(next_id, schedule_requested_token_);
        if (!coordinator_->continue_request(request_id, next_id, next_body)) {
            finish_schedule_failure(QStringLiteral("schedule.query_failed"));
            return RequestDisposition::kFinished;
        }
        schedule_request_id_ = next_id;
        return RequestDisposition::kContinue;
    }
    auto loaded = pending_schedules_;
    pending_schedules_.clear();
    schedules_ = std::move(loaded);
    enrich_schedules_with_connections();
    schedule_list_.set_items(schedules_);
    schedules_loading_ = false;
    schedule_request_id_.clear();
    schedule_requested_token_.reset();
    emit schedulesChanged();
    emit loadingChanged();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_schedule_failure(const QString& message_code) {
    schedules_error_code_ = message_code;
    schedules_loading_ = false;
    schedule_request_id_.clear();
    schedule_requested_token_.reset();
    pending_schedules_.clear();
    emit schedulesChanged();
    emit loadingChanged();
}

void ServiceClient::reset_schedules() {
    schedules_.clear();
    pending_schedules_.clear();
    schedule_list_.clear();
    schedules_loading_ = false;
    schedules_error_code_.clear();
    schedule_request_id_.clear();
    schedule_requested_token_.reset();
    schedule_command_request_id_.clear();
    schedule_command_idempotency_key_.clear();
    schedule_command_kind_ = 0;
    schedule_enable_patch_active_ = false;
    schedule_enable_patch_id_.clear();
    emit schedulesChanged();
    if (schedule_command_busy_) {
        set_schedule_command_busy(false);
        emit scheduleCommandFailed(localize_message_code(QStringLiteral("schedule.command_failed")));
    }
}

bool ServiceClient::upsertSchedule(const QString& schedule_id, const QString& display_name,
                                   const bool enabled, const QVariantList& source_ids,
                                   const QString& connection_id, const QString& frequency,
                                   const QString& time_of_day,
                                   const bool exclude_page_and_hibernation_files,
                                   const bool deduplication_enabled,
                                   const bool encryption_enabled,
                                   const QString& archive_password, const int backup_type,
                                   const int weekday_mask, const unsigned int day_of_month_mask) {
    if (state_ != State::kReady || !schedules_available_ || schedule_command_busy_ ||
        source_ids.isEmpty() || source_ids.size() > 100 || connection_id.isEmpty() ||
        display_name.isEmpty()) {
        return false;
    }
    if (backup_type != kBackupTypeFull && backup_type != kBackupTypeIncremental) {
        return false;
    }
    // Create may set encryption + password; update must not send password material and must keep
    // encryption / sources / other options aligned with the durable schedule (Service enforces).
    if (!schedule_id.isEmpty() && !archive_password.isEmpty()) {
        return false;
    }
    if (encryption_enabled) {
        if (schedule_id.isEmpty() &&
            (archive_password.isEmpty() || archive_password.size() > 32)) {
            return false;
        }
    } else if (!archive_password.isEmpty()) {
        return false;
    }
    int trigger_kind = kScheduleTriggerDaily;
    if (frequency.compare(QStringLiteral("weekly"), Qt::CaseInsensitive) == 0) {
        trigger_kind = kScheduleTriggerWeekly;
    } else if (frequency.compare(QStringLiteral("monthly"), Qt::CaseInsensitive) == 0) {
        trigger_kind = kScheduleTriggerMonthly;
    }
    const auto local_minutes = parse_times_of_day_minutes(time_of_day);
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto idempotency_key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    schedule_command_request_id_ = request_id;
    schedule_command_idempotency_key_ = idempotency_key;
    schedule_command_kind_ = kUpsertScheduleRequestKind;
    set_schedule_command_busy(true);
    const auto body = encode_upsert_schedule_request(
        request_id, idempotency_key, schedule_id, display_name, enabled, source_ids, connection_id,
        backup_type, trigger_kind, local_minutes, weekday_mask, QStringLiteral("UTC"),
        exclude_page_and_hibernation_files, deduplication_enabled, encryption_enabled,
        archive_password, day_of_month_mask);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_schedule_command_frame(frame_body);
        });
    if (!started) {
        finish_schedule_command_failure(QStringLiteral("schedule.command_failed"));
        return false;
    }
    return true;
}

bool ServiceClient::createSchedule(const QVariantList& sources, const QString& connection_id,
                                   const QString& frequency, const QString& time_of_day,
                                   const bool exclude_page_and_hibernation_files,
                                   const bool deduplication_enabled,
                                   const bool encryption_enabled,
                                   const QString& archive_password,
                                   const bool start_full_backup_after_create,
                                   const int weekday_mask, const unsigned int day_of_month_mask) {
    if (state_ != State::kReady || !schedules_available_ || schedule_command_busy_ ||
        sources.isEmpty() || sources.size() > 100 || connection_id.isEmpty()) {
        return false;
    }
    QSet<QString> seen_source_ids;
    QVariantList source_ids;
    QStringList display_names;
    for (const auto& source : sources) {
        const auto item = source.toMap();
        const auto source_id = item.value(QStringLiteral("sourceId")).toString();
        const auto display_name = item.value(QStringLiteral("displayName")).toString();
        if (source_id.isEmpty() || display_name.isEmpty() || seen_source_ids.contains(source_id)) {
            return false;
        }
        seen_source_ids.insert(source_id);
        source_ids.push_back(source_id);
        display_names.push_back(display_name);
    }
    start_full_backup_after_schedule_create_ = start_full_backup_after_create;
    const auto started =
        upsertSchedule({}, display_names.join(QStringLiteral(", ")), true, source_ids,
                       connection_id, frequency, time_of_day, exclude_page_and_hibernation_files,
                       deduplication_enabled, encryption_enabled, archive_password,
                       kBackupTypeIncremental, weekday_mask, day_of_month_mask);
    if (!started) {
        start_full_backup_after_schedule_create_ = false;
    }
    return started;
}

bool ServiceClient::deleteSchedule(const QString& schedule_id) {
    if (state_ != State::kReady || !schedules_available_ || schedule_command_busy_ ||
        schedule_id.isEmpty()) {
        return false;
    }
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto idempotency_key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    schedule_command_request_id_ = request_id;
    schedule_command_idempotency_key_ = idempotency_key;
    schedule_command_kind_ = kDeleteScheduleRequestKind;
    set_schedule_command_busy(true);
    const auto body = encode_delete_schedule_request(request_id, idempotency_key, schedule_id);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_schedule_command_frame(frame_body);
        });
    if (!started) {
        finish_schedule_command_failure(QStringLiteral("schedule.command_failed"));
        return false;
    }
    return true;
}

bool ServiceClient::setScheduleEnabled(const QString& schedule_id, const bool enabled) {
    QVariantMap found;
    for (const auto& item : schedules_) {
        const auto map = item.toMap();
        if (map.value(QStringLiteral("scheduleId")).toString() == schedule_id ||
            map.value(QStringLiteral("id")).toString() == schedule_id) {
            found = map;
            break;
        }
    }
    if (found.isEmpty()) {
        return false;
    }
    const auto previous = found.value(QStringLiteral("enabled")).toBool();
    if (previous == enabled) {
        return true;
    }
    // Optimistic row patch: UI updates immediately without ListSchedules / loading flash.
    schedule_enable_patch_active_ = true;
    schedule_enable_patch_id_ = schedule_id;
    schedule_enable_patch_previous_ = previous;
    schedule_enable_patch_target_ = enabled;
    if (!patch_schedule_enabled(schedule_id, enabled)) {
        schedule_enable_patch_active_ = false;
        schedule_enable_patch_id_.clear();
        return false;
    }
    const auto exclude = found.value(QStringLiteral("excludePageAndHibernation"), true).toBool();
    const auto dedup = found.value(QStringLiteral("deduplicationEnabled"), true).toBool();
    const auto encryption = found.value(QStringLiteral("encryptionEnabled"), false).toBool();
    const auto weekday_mask = found.value(QStringLiteral("weekdayMask"), 0).toInt();
    const auto day_of_month_mask =
        static_cast<unsigned int>(found.value(QStringLiteral("dayOfMonthMask"), 0).toUInt());
    const auto display_name = found.value(QStringLiteral("displayName")).toString();
    const auto connection_id = found.value(QStringLiteral("connectionId")).toString();
    const auto frequency = found.value(QStringLiteral("frequency")).toString();
    const auto time_of_day = found.value(QStringLiteral("timeOfDay")).toString();
    // Preserve create-time sources, options, and encryption; Service stores Incremental.
    bool started = false;
    if (found.value(QStringLiteral("contentKind")).toInt() == 2) {
        started = updateFileSetSchedule(schedule_id, display_name, enabled, connection_id,
                                        frequency, time_of_day, exclude, encryption, weekday_mask,
                                        day_of_month_mask);
    } else {
        started = upsertSchedule(schedule_id, display_name, enabled,
                                 found.value(QStringLiteral("sourceIds")).toList(), connection_id,
                                 frequency, time_of_day, exclude, dedup, encryption, {},
                                 kBackupTypeIncremental, weekday_mask, day_of_month_mask);
    }
    if (!started) {
        // upsert/update already cleared busy; restore prior enabled for this row.
        schedule_enable_patch_active_ = false;
        (void)patch_schedule_enabled(schedule_id, previous);
        schedule_enable_patch_id_.clear();
        return false;
    }
    return true;
}

RequestDisposition ServiceClient::handle_schedule_command_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, schedule_command_kind_)) {
        finish_schedule_command_failure(QStringLiteral("schedule.command_failed"));
        return RequestDisposition::kFinished;
    }
    CommandAck ack;
    if (!parse_command_ack_response(root, schedule_command_kind_, ack)) {
        return RequestDisposition::kProtocolError;
    }
    const auto created_kind = schedule_command_kind_ == kUpsertScheduleRequestKind;
    const auto run_after_create = start_full_backup_after_schedule_create_;
    const auto created_schedule_id = ack.has_resource_id ? ack.resource_id : QString{};
    const auto enable_patch = schedule_enable_patch_active_;
    schedule_command_request_id_.clear();
    schedule_command_idempotency_key_.clear();
    schedule_command_kind_ = 0;
    start_full_backup_after_schedule_create_ = false;
    // Clear busy before dropping enable_patch so globalLoading never flashes true.
    set_schedule_command_busy(false);
    schedule_enable_patch_active_ = false;
    schedule_enable_patch_id_.clear();
    emit scheduleCommandSucceeded();
    if (enable_patch) {
        // Toggle already applied optimistically; skip full ListSchedules refresh.
    } else {
        // Create / edit / delete still reload authoritative list from Service.
        start_schedule_query();
    }
    // Wizard "create then run": request Incremental; first run demotes to Full.
    if (created_kind && run_after_create && !created_schedule_id.isEmpty()) {
        (void)startBackup(created_schedule_id, kBackupTypeIncremental);
    }
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_schedule_command_failure(const QString& message_code) {
    const auto was_enable_patch = schedule_enable_patch_active_;
    if (was_enable_patch && !schedule_enable_patch_id_.isEmpty()) {
        (void)patch_schedule_enabled(schedule_enable_patch_id_, schedule_enable_patch_previous_);
    }
    schedule_enable_patch_active_ = false;
    schedule_enable_patch_id_.clear();
    schedule_command_request_id_.clear();
    schedule_command_idempotency_key_.clear();
    schedule_command_kind_ = 0;
    start_full_backup_after_schedule_create_ = false;
    schedules_error_code_ = message_code;
    // Enable-toggle failures already row-patched; avoid schedulesChanged full-property churn.
    if (!was_enable_patch) {
        emit schedulesChanged();
    }
    show_toast(message_code, true);
    emit scheduleCommandFailed(localize_message_code(message_code));
    set_schedule_command_busy(false);
}

} // namespace aegra::desktop
