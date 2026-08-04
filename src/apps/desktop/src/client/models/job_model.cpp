#include "client/models/job_model.h"

#include "locale/locale_format.h"
#include "locale/message_code_map.h"

#include <QVariantMap>

#include <algorithm>
#include <limits>

namespace aegra::desktop {
namespace {

constexpr std::int64_t kStateQueued = 1;
constexpr std::int64_t kStateRunning = 2;
constexpr std::int64_t kStateCancelling = 3;
constexpr std::int64_t kStateSucceeded = 4;
constexpr std::int64_t kStateFailed = 5;
constexpr std::int64_t kStateCancelled = 6;
constexpr std::int64_t kStateInterrupted = 7;

} // namespace

JobModel::JobModel(QObject* parent) : QAbstractListModel(parent) {}

void JobModel::set_locale_format(LocaleFormat* format) { format_ = format; }

void JobModel::set_rows(QVector<JobRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    running_count_ = 0;
    failed_count_ = 0;
    succeeded_count_ = 0;
    active_count_ = 0;
    for (const auto& row : rows_) {
        if (row.state == kStateRunning) {
            ++running_count_;
        } else if (row.state == kStateFailed) {
            ++failed_count_;
        } else if (row.state == kStateSucceeded) {
            ++succeeded_count_;
        }
        if (is_active_state(row.state)) {
            ++active_count_;
        }
    }
    endResetModel();
    emit countChanged();
    emit countsChanged();
}

void JobModel::clear() {
    if (rows_.isEmpty()) {
        return;
    }
    beginResetModel();
    rows_.clear();
    running_count_ = 0;
    failed_count_ = 0;
    succeeded_count_ = 0;
    active_count_ = 0;
    endResetModel();
    emit countChanged();
    emit countsChanged();
}

void JobModel::retranslate() {
    if (rows_.isEmpty()) {
        return;
    }
    emit dataChanged(index(0, 0), index(rows_.size() - 1, 0));
}

int JobModel::runningCount() const noexcept { return running_count_; }

int JobModel::failedCount() const noexcept { return failed_count_; }

int JobModel::succeededCount() const noexcept { return succeeded_count_; }

int JobModel::activeCount() const noexcept { return active_count_; }

bool JobModel::has_active_jobs() const noexcept { return active_count_ > 0; }

std::optional<JobRow> JobModel::find_job(const QString& job_id) const {
    for (const auto& row : rows_) {
        if (row.job_id == job_id) {
            return row;
        }
    }
    return std::nullopt;
}

int JobModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant JobModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    const auto& row = rows_.at(index.row());
    switch (role) {
    case JobIdRole:
        return row.job_id;
    case TraceIdRole:
        return row.trace_id;
    case OperationTextRole:
        return operation_text(row.operation);
    case StateValueRole:
        return static_cast<qint64>(row.state);
    case StateTextRole:
        return state_text(row.state);
    case CreatedTextRole:
        return format_ != nullptr ? format_->format_date_time_utc_ms(row.created_utc_ms)
                                  : QString::number(row.created_utc_ms);
    case ProgressPercentRole:
        return progress_percent(row);
    case ProgressVisibleRole:
        return is_active_state(row.state) && row.progress_processed_bytes.has_value();
    case MessageTextRole:
        return row.message_code.isEmpty() ? QString{} : localize_message_code(row.message_code);
    case IsTerminalRole:
        return is_terminal_state(row.state);
    case IsActiveRole:
        return is_active_state(row.state);
    default:
        return {};
    }
}

QHash<int, QByteArray> JobModel::roleNames() const {
    return {{JobIdRole, "jobId"},
            {TraceIdRole, "traceId"},
            {OperationTextRole, "operationText"},
            {StateValueRole, "stateValue"},
            {StateTextRole, "stateText"},
            {CreatedTextRole, "createdText"},
            {ProgressPercentRole, "progressPercent"},
            {ProgressVisibleRole, "progressVisible"},
            {MessageTextRole, "messageText"},
            {IsTerminalRole, "isTerminal"},
            {IsActiveRole, "isActive"}};
}

QString JobModel::operation_text(const std::int64_t operation) const {
    switch (operation) {
    case 1:
        //% "Backup"
        return qtTrId("aegra.job.operation.backup");
    case 2:
        //% "Restore"
        return qtTrId("aegra.job.operation.restore");
    case 3:
        //% "Verify"
        return qtTrId("aegra.job.operation.verify");
    case 4:
        //% "Export"
        return qtTrId("aegra.job.operation.export");
    default:
        //% "Unknown"
        return qtTrId("aegra.common.unknown");
    }
}

QString JobModel::state_text(const std::int64_t state) const {
    switch (state) {
    case kStateQueued:
        //% "Queued"
        return qtTrId("aegra.task.state.queued");
    case kStateRunning:
        //% "Running"
        return qtTrId("aegra.task.state.running");
    case kStateCancelling:
        //% "Cancelling"
        return qtTrId("aegra.task.state.cancelling");
    case kStateSucceeded:
        //% "Succeeded"
        return qtTrId("aegra.task.state.succeeded");
    case kStateFailed:
        //% "Failed"
        return qtTrId("aegra.task.state.failed");
    case kStateCancelled:
        //% "Cancelled"
        return qtTrId("aegra.task.state.cancelled");
    case kStateInterrupted:
        //% "Interrupted"
        return qtTrId("aegra.task.state.interrupted");
    default:
        //% "Unknown"
        return qtTrId("aegra.common.unknown");
    }
}

bool JobModel::is_terminal_state(const std::int64_t state) noexcept {
    return state == kStateSucceeded || state == kStateFailed || state == kStateCancelled ||
           state == kStateInterrupted;
}

bool JobModel::is_active_state(const std::int64_t state) noexcept {
    return state == kStateQueued || state == kStateRunning || state == kStateCancelling;
}

int JobModel::progress_percent(const JobRow& row) noexcept {
    if (!row.progress_logical_bytes || !row.progress_processed_bytes) {
        return 0;
    }
    const auto logical = *row.progress_logical_bytes;
    const auto processed = *row.progress_processed_bytes;
    if (logical <= 0 || processed < 0 || processed > logical) {
        return 0;
    }
    if (processed == logical) {
        return 100;
    }
    // Overflow-safe percent in [0, 100]. Never multiply processed by 100 when it may exceed
    // int64_t::max / 100; scale the denominator instead when values are large.
    std::int64_t percent = 0;
    constexpr auto kMax = (std::numeric_limits<std::int64_t>::max)();
    if (processed <= kMax / 100) {
        percent = (processed * 100) / logical;
    } else if (logical >= 100) {
        percent = processed / (logical / 100);
    } else {
        // logical in [1, 99] and processed is huge but still <= logical — impossible for
        // processed > kMax/100 with logical < 100. Keep a defensive fallback.
        percent = (processed * 100) / logical;
    }
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return static_cast<int>(percent);
}

QVector<JobRow> jobs_from_variant_list(const QVariantList& items) {
    QVector<JobRow> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        const auto map = item.toMap();
        JobRow row;
        row.job_id = map.value(QStringLiteral("jobId")).toString();
        row.trace_id = map.value(QStringLiteral("traceId")).toString();
        row.operation = map.value(QStringLiteral("operation")).toLongLong();
        row.state = map.value(QStringLiteral("state")).toLongLong();
        row.created_utc_ms = map.value(QStringLiteral("createdUtcMs")).toLongLong();
        if (map.contains(QStringLiteral("startedUtcMs")) &&
            map.value(QStringLiteral("startedUtcMs")).isValid()) {
            row.started_utc_ms = map.value(QStringLiteral("startedUtcMs")).toLongLong();
        }
        if (map.contains(QStringLiteral("completedUtcMs")) &&
            map.value(QStringLiteral("completedUtcMs")).isValid()) {
            row.completed_utc_ms = map.value(QStringLiteral("completedUtcMs")).toLongLong();
        }
        if (map.contains(QStringLiteral("progressPhase")) &&
            map.value(QStringLiteral("progressPhase")).isValid()) {
            row.progress_phase = map.value(QStringLiteral("progressPhase")).toLongLong();
        }
        if (map.contains(QStringLiteral("progressLogicalBytes")) &&
            map.value(QStringLiteral("progressLogicalBytes")).isValid()) {
            row.progress_logical_bytes =
                map.value(QStringLiteral("progressLogicalBytes")).toLongLong();
        }
        if (map.contains(QStringLiteral("progressProcessedBytes")) &&
            map.value(QStringLiteral("progressProcessedBytes")).isValid()) {
            row.progress_processed_bytes =
                map.value(QStringLiteral("progressProcessedBytes")).toLongLong();
        }
        if (map.contains(QStringLiteral("progressStoredBytes")) &&
            map.value(QStringLiteral("progressStoredBytes")).isValid()) {
            row.progress_stored_bytes =
                map.value(QStringLiteral("progressStoredBytes")).toLongLong();
        }
        row.message_code = map.value(QStringLiteral("messageCode")).toString();
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace aegra::desktop
