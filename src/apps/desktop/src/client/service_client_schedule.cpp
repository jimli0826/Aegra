#include "client/service_client.h"

#include "client/service_protocol.h"

#include <QDateTime>
#include <QJsonObject>
#include <QSet>
#include <QTimeZone>
#include <QUuid>
#include <QVariantMap>

namespace aegra::desktop {
namespace {

constexpr int kMaximumSchedules = 500;

[[nodiscard]] int parse_time_of_day_minutes(const QString& time_of_day) {
    const auto parts = time_of_day.split(QLatin1Char(':'));
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
    return hour * 60 + minute;
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

bool ServiceClient::schedulesLoading() const noexcept { return schedules_loading_; }

bool ServiceClient::schedulesAvailable() const noexcept { return schedules_available_; }

QString ServiceClient::schedulesErrorText() const {
    if (schedules_error_code_.isEmpty()) {
        return {};
    }
    return schedules_error_code_;
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

void ServiceClient::start_schedule_query() {
    if (!schedules_available_) {
        return;
    }
    schedules_error_code_.clear();
    schedules_loading_ = true;
    pending_schedules_.clear();
    schedule_requested_token_.reset();
    emit schedulesChanged();

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
        return RequestDisposition::kProtocolError;
    }
    if ((page.continuation_token && page.continuation_token == schedule_requested_token_) ||
        pending_schedules_.size() + page.items.size() > kMaximumSchedules) {
        return RequestDisposition::kProtocolError;
    }
    QSet<QString> seen_ids;
    for (const auto& existing : pending_schedules_) {
        seen_ids.insert(existing.toMap().value(QStringLiteral("scheduleId")).toString());
    }
    for (auto& item : page.items) {
        const auto schedule_id = item.toMap().value(QStringLiteral("scheduleId")).toString();
        if (seen_ids.contains(schedule_id)) {
            return RequestDisposition::kProtocolError;
        }
        seen_ids.insert(schedule_id);
        pending_schedules_.push_back(std::move(item));
    }
    if (page.continuation_token) {
        schedule_requested_token_ = page.continuation_token;
        const auto next_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto next_body = encode_schedule_list_request(next_id, schedule_requested_token_);
        if (!coordinator_->continue_request(request_id, next_id, next_body)) {
            return RequestDisposition::kProtocolError;
        }
        schedule_request_id_ = next_id;
        return RequestDisposition::kContinue;
    }
    schedules_ = pending_schedules_;
    pending_schedules_.clear();
    enrich_schedules_with_connections();
    schedules_loading_ = false;
    schedule_request_id_.clear();
    schedule_requested_token_.reset();
    emit schedulesChanged();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_schedule_failure(const QString& message_code) {
    schedules_error_code_ = message_code;
    schedules_loading_ = false;
    schedule_request_id_.clear();
    schedule_requested_token_.reset();
    pending_schedules_.clear();
    emit schedulesChanged();
}

void ServiceClient::reset_schedules() {
    schedules_.clear();
    pending_schedules_.clear();
    schedules_loading_ = false;
    schedules_error_code_.clear();
    schedule_request_id_.clear();
    schedule_requested_token_.reset();
    schedule_command_busy_ = false;
    schedule_command_request_id_.clear();
    schedule_command_idempotency_key_.clear();
    schedule_command_kind_ = 0;
    emit schedulesChanged();
}

bool ServiceClient::upsertSchedule(const QString& schedule_id, const QString& display_name,
                                   const bool enabled, const QVariantList& source_ids,
                                   const QString& connection_id, const QString& frequency,
                                   const QString& time_of_day) {
    if (state_ != State::kReady || !schedules_available_ || schedule_command_busy_ ||
        source_ids.isEmpty() || source_ids.size() > 100 || connection_id.isEmpty() ||
        display_name.isEmpty()) {
        return false;
    }
    const auto trigger_kind =
        frequency.compare(QStringLiteral("weekly"), Qt::CaseInsensitive) == 0
            ? kScheduleTriggerWeekly
            : kScheduleTriggerDaily;
    const auto local_minute = parse_time_of_day_minutes(time_of_day);
    const auto request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto idempotency_key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    schedule_command_request_id_ = request_id;
    schedule_command_idempotency_key_ = idempotency_key;
    schedule_command_kind_ = kUpsertScheduleRequestKind;
    schedule_command_busy_ = true;
    const auto body = encode_upsert_schedule_request(
        request_id, idempotency_key, schedule_id, display_name, enabled, source_ids, connection_id,
        kBackupTypeFull, trigger_kind, local_minute, 0, QStringLiteral("UTC"));
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
                                   const QString& frequency, const QString& time_of_day) {
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
    return upsertSchedule({}, display_names.join(QStringLiteral(", ")), true, source_ids,
                          connection_id, frequency, time_of_day);
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
    schedule_command_busy_ = true;
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
        if (map.value(QStringLiteral("scheduleId")).toString() == schedule_id) {
            found = map;
            break;
        }
    }
    if (found.isEmpty()) {
        return false;
    }
    return upsertSchedule(schedule_id, found.value(QStringLiteral("displayName")).toString(),
                          enabled, found.value(QStringLiteral("sourceIds")).toList(),
                          found.value(QStringLiteral("connectionId")).toString(),
                          found.value(QStringLiteral("frequency")).toString(),
                          found.value(QStringLiteral("timeOfDay")).toString());
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
    schedule_command_request_id_.clear();
    schedule_command_idempotency_key_.clear();
    schedule_command_kind_ = 0;
    // Reload authoritative list from Service after mutation.
    schedule_command_busy_ = false;
    start_schedule_query();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_schedule_command_failure(const QString& message_code) {
    schedule_command_busy_ = false;
    schedule_command_request_id_.clear();
    schedule_command_idempotency_key_.clear();
    schedule_command_kind_ = 0;
    schedules_error_code_ = message_code;
    emit schedulesChanged();
    show_toast(message_code);
}

} // namespace aegra::desktop
