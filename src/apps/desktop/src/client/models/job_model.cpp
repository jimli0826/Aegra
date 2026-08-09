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

[[nodiscard]] bool row_is_active(const std::int64_t state) noexcept {
    return state == kStateQueued || state == kStateRunning || state == kStateCancelling;
}

void recompute_counts(const QVector<JobRow>& rows, int& running, int& failed, int& succeeded,
                      int& active) {
    running = 0;
    failed = 0;
    succeeded = 0;
    active = 0;
    for (const auto& row : rows) {
        if (row.state == kStateRunning) {
            ++running;
        } else if (row.state == kStateFailed) {
            ++failed;
        } else if (row.state == kStateSucceeded) {
            ++succeeded;
        }
        if (row_is_active(row.state)) {
            ++active;
        }
    }
}

} // namespace

JobModel::JobModel(QObject* parent) : QAbstractListModel(parent) {}

void JobModel::set_locale_format(LocaleFormat* format) { format_ = format; }

void JobModel::set_rows(QVector<JobRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    recompute_counts(rows_, running_count_, failed_count_, succeeded_count_, active_count_);
    endResetModel();
    emit countChanged();
    emit countsChanged();
    bump_revision();
}

void JobModel::upsert_job(JobRow row) {
    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_[i].job_id == row.job_id) {
            rows_[i] = std::move(row);
            const auto idx = index(i, 0);
            emit dataChanged(idx, idx);
            recompute_counts(rows_, running_count_, failed_count_, succeeded_count_, active_count_);
            emit countsChanged();
            bump_revision();
            return;
        }
    }
    beginInsertRows(QModelIndex(), 0, 0);
    rows_.prepend(std::move(row));
    endInsertRows();
    recompute_counts(rows_, running_count_, failed_count_, succeeded_count_, active_count_);
    emit countChanged();
    emit countsChanged();
    bump_revision();
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
    bump_revision();
}

void JobModel::retranslate() {
    if (rows_.isEmpty()) {
        return;
    }
    emit dataChanged(index(0, 0), index(rows_.size() - 1, 0));
    bump_revision();
}

int JobModel::runningCount() const noexcept { return running_count_; }

int JobModel::failedCount() const noexcept { return failed_count_; }

int JobModel::succeededCount() const noexcept { return succeeded_count_; }

int JobModel::activeCount() const noexcept { return active_count_; }

int JobModel::revision() const noexcept { return revision_; }

void JobModel::bump_revision() {
    ++revision_;
    emit revisionChanged();
}

bool JobModel::has_active_jobs() const noexcept { return active_count_ > 0; }

std::optional<JobRow> JobModel::find_job(const QString& job_id) const {
    for (const auto& row : rows_) {
        if (row.job_id == job_id) {
            return row;
        }
    }
    return std::nullopt;
}

namespace {

constexpr std::int64_t kOperationBackup = 1;

[[nodiscard]] bool source_ids_overlap(const QStringList& job_sources,
                                      const QVariantList& schedule_sources) {
    if (job_sources.isEmpty() || schedule_sources.isEmpty()) {
        return false;
    }
    for (const auto& schedule_source : schedule_sources) {
        const auto id = schedule_source.toString();
        if (!id.isEmpty() && job_sources.contains(id)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] QString status_key_for_state(const std::int64_t state) noexcept {
    if (state == kStateQueued || state == kStateRunning || state == kStateCancelling) {
        return QStringLiteral("running");
    }
    if (state == kStateSucceeded) {
        return QStringLiteral("success");
    }
    if (state == kStateFailed || state == kStateCancelled || state == kStateInterrupted) {
        return QStringLiteral("failed");
    }
    return QStringLiteral("none");
}

} // namespace

QVariantMap JobModel::latestBackupStatus(const QVariantList& source_ids,
                                         const QString& connection_id) const {
    QVariantMap empty{{QStringLiteral("statusKey"), QStringLiteral("none")},
                      {QStringLiteral("progressPercent"), 0},
                      {QStringLiteral("stateText"), QString{}},
                      {QStringLiteral("stateValue"), 0}};
    if (connection_id.isEmpty() || source_ids.isEmpty()) {
        return empty;
    }

    const JobRow* best_active = nullptr;
    const JobRow* best_terminal = nullptr;
    for (const auto& row : rows_) {
        if (row.operation != kOperationBackup || row.connection_id != connection_id) {
            continue;
        }
        if (!source_ids_overlap(row.source_ids, source_ids)) {
            continue;
        }
        if (is_active_state(row.state)) {
            if (best_active == nullptr || row.created_utc_ms >= best_active->created_utc_ms) {
                best_active = &row;
            }
            continue;
        }
        if (is_terminal_state(row.state)) {
            if (best_terminal == nullptr || row.created_utc_ms >= best_terminal->created_utc_ms) {
                best_terminal = &row;
            }
        }
    }

    const JobRow* chosen = best_active != nullptr ? best_active : best_terminal;
    if (chosen == nullptr) {
        return empty;
    }
    QVariantMap result{{QStringLiteral("statusKey"), status_key_for_state(chosen->state)},
                       {QStringLiteral("progressPercent"), progress_percent(*chosen)},
                       {QStringLiteral("stateText"), state_text(chosen->state)},
                       {QStringLiteral("stateValue"), static_cast<qint64>(chosen->state)}};
    if (chosen->requested_backup_type) {
        result.insert(QStringLiteral("requestedBackupTypeText"),
                      backup_type_text(*chosen->requested_backup_type));
    }
    if (chosen->effective_backup_type) {
        result.insert(QStringLiteral("effectiveBackupTypeText"),
                      backup_type_text(*chosen->effective_backup_type));
    }
    if (chosen->incremental_downgrade_reason) {
        result.insert(QStringLiteral("hasDowngrade"), true);
        result.insert(QStringLiteral("downgradeReasonText"),
                      downgrade_reason_text(*chosen->incremental_downgrade_reason));
    } else {
        result.insert(QStringLiteral("hasDowngrade"), false);
    }
    if (!chosen->message_code.isEmpty()) {
        result.insert(QStringLiteral("messageText"), localize_message_code(chosen->message_code));
    }
    return result;
}

QVariantMap JobModel::restoreSessionStatus(const qint64 since_utc_ms) const {
    constexpr std::int64_t kOperationRestore = 2;
    int job_count = 0;
    int active_count = 0;
    int terminal_count = 0;
    int failed_count = 0;
    int pct_sum = 0;
    QString state_text_out;
    QString message_text_out;
    QString source_name_out;

    for (const auto& row : rows_) {
        if (row.operation != kOperationRestore) {
            continue;
        }
        if (since_utc_ms > 0 && row.created_utc_ms < since_utc_ms) {
            continue;
        }
        ++job_count;
        pct_sum += progress_percent(row);
        if (is_active_state(row.state)) {
            ++active_count;
            if (state_text_out.isEmpty()) {
                state_text_out = state_text(row.state);
            }
            if (message_text_out.isEmpty() && !row.message_code.isEmpty()) {
                message_text_out = localize_message_code(row.message_code);
            }
            if (source_name_out.isEmpty()) {
                source_name_out =
                    row.source_name.isEmpty()
                        ? (row.source_ids.isEmpty() ? operation_text(row.operation)
                                                    : row.source_ids.join(QStringLiteral(", ")))
                        : row.source_name;
            }
        } else if (is_terminal_state(row.state)) {
            ++terminal_count;
            if (row.state == kStateFailed || row.state == kStateInterrupted ||
                row.state == kStateCancelled) {
                ++failed_count;
            }
            if (state_text_out.isEmpty()) {
                state_text_out = state_text(row.state);
            }
            if (message_text_out.isEmpty() && !row.message_code.isEmpty()) {
                message_text_out = localize_message_code(row.message_code);
            }
            if (source_name_out.isEmpty()) {
                source_name_out =
                    row.source_name.isEmpty()
                        ? (row.source_ids.isEmpty() ? operation_text(row.operation)
                                                    : row.source_ids.join(QStringLiteral(", ")))
                        : row.source_name;
            }
        }
    }

    const bool all_terminal =
        job_count > 0 && active_count == 0 && terminal_count == job_count;
    QString status_key = QStringLiteral("none");
    if (active_count > 0) {
        status_key = QStringLiteral("running");
    } else if (all_terminal && failed_count > 0) {
        status_key = QStringLiteral("failed");
    } else if (all_terminal) {
        status_key = QStringLiteral("success");
    }

    const int progress = job_count > 0 ? pct_sum / job_count : 0;
    return {{QStringLiteral("jobCount"), job_count},
            {QStringLiteral("activeCount"), active_count},
            {QStringLiteral("progressPercent"), progress},
            {QStringLiteral("stateText"), state_text_out},
            {QStringLiteral("messageText"), message_text_out},
            {QStringLiteral("sourceName"), source_name_out},
            {QStringLiteral("statusKey"), status_key},
            {QStringLiteral("allTerminal"), all_terminal},
            {QStringLiteral("anyFailed"), failed_count > 0}};
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
    case StateColorRole:
        return state_color(row.state);
    case CreatedTextRole:
        return format_ != nullptr ? format_->format_date_time_utc_ms(row.created_utc_ms)
                                  : QString::number(row.created_utc_ms);
    case ProgressPercentRole:
        return progress_percent(row);
    case ProgressVisibleRole:
        return progress_visible(row);
    case MessageTextRole:
        return row.message_code.isEmpty() ? QString{} : localize_message_code(row.message_code);
    case IsTerminalRole:
        return is_terminal_state(row.state);
    case IsActiveRole:
        return is_active_state(row.state);
    case SourceNameRole:
        return row.source_name.isEmpty()
                   ? (row.source_ids.isEmpty() ? operation_text(row.operation)
                                               : row.source_ids.join(QStringLiteral(", ")))
                   : row.source_name;
    case DestinationNameRole:
        return row.destination_name.isEmpty()
                   ? (row.connection_id.isEmpty() ? row.job_id : row.connection_id)
                   : row.destination_name;
    case DestinationPathRole:
        return row.destination_path;
    case SourceIdsRole:
        return row.source_ids;
    case ConnectionIdRole:
        return row.connection_id;
    case RequestedBackupTypeTextRole:
        return row.requested_backup_type ? backup_type_text(*row.requested_backup_type) : QString{};
    case EffectiveBackupTypeTextRole:
        return row.effective_backup_type ? backup_type_text(*row.effective_backup_type) : QString{};
    case DowngradeReasonTextRole:
        return row.incremental_downgrade_reason
                   ? downgrade_reason_text(*row.incremental_downgrade_reason)
                   : QString{};
    case HasDowngradeRole:
        return row.incremental_downgrade_reason.has_value();
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
            {StateColorRole, "stateColor"},
            {CreatedTextRole, "createdText"},
            {ProgressPercentRole, "progressPercent"},
            {ProgressVisibleRole, "progressVisible"},
            {MessageTextRole, "messageText"},
            {IsTerminalRole, "isTerminal"},
            {IsActiveRole, "isActive"},
            {SourceNameRole, "sourceName"},
            {DestinationNameRole, "destinationName"},
            {DestinationPathRole, "destinationPath"},
            {SourceIdsRole, "sourceIds"},
            {ConnectionIdRole, "connectionId"},
            {RequestedBackupTypeTextRole, "requestedBackupTypeText"},
            {EffectiveBackupTypeTextRole, "effectiveBackupTypeText"},
            {DowngradeReasonTextRole, "downgradeReasonText"},
            {HasDowngradeRole, "hasDowngrade"}};
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

QString JobModel::backup_type_text(const std::int64_t backup_type) const {
    switch (backup_type) {
    case 1:
        //% "Full"
        return qtTrId("aegra.backup.type.full");
    case 2:
        //% "Incremental"
        return qtTrId("aegra.backup.type.incremental");
    case 3:
        //% "Differential"
        return qtTrId("aegra.backup.type.differential");
    default:
        //% "Unknown"
        return qtTrId("aegra.common.unknown");
    }
}

QString JobModel::downgrade_reason_text(const std::int64_t reason) const {
    // Maps contracts::IncrementalDowngradeReason (1, 2, 3, or 9) to localized copy.
    // Desktop never invents authority — only surfaces Service-projected reason codes.
    switch (reason) {
    case 1:
        //% "No eligible parent recovery point; a new full baseline was created."
        return qtTrId("aegra.backup.downgrade.no_parent");
    case 2:
        //% "Backup selection changed; a new full baseline was created."
        return qtTrId("aegra.backup.downgrade.selection_changed");
    case 3:
        //% "Parent backup chain is incomplete; a new full baseline was created."
        return qtTrId("aegra.backup.downgrade.chain_incomplete");
    case 9:
        //% "Parent metadata baseline is invalid; a new full baseline was created."
        return qtTrId("aegra.backup.downgrade.metadata_baseline_invalid");
    default:
        //% "Incremental was not eligible; a full backup was created instead."
        return qtTrId("aegra.backup.downgrade.generic");
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

QString JobModel::state_color(const std::int64_t state) noexcept {
    switch (state) {
    case kStateSucceeded:
        return QStringLiteral("#3dd68c");
    case kStateFailed:
        return QStringLiteral("#e5534b");
    case kStateCancelled:
    case kStateInterrupted:
        return QStringLiteral("#e6a817");
    case kStateRunning:
    case kStateCancelling:
        return QStringLiteral("#33b8ff");
    default:
        return QStringLiteral("#e8eef7");
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
    if (row.state == kStateSucceeded) {
        return 100;
    }
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
    // Overflow-safe percent in [0, 100].
    std::int64_t percent = 0;
    constexpr auto kMax = (std::numeric_limits<std::int64_t>::max)();
    if (processed <= kMax / 100) {
        percent = (processed * 100) / logical;
    } else if (logical >= 100) {
        percent = processed / (logical / 100);
    } else {
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

bool JobModel::progress_visible(const JobRow& row) noexcept {
    if (row.state == kStateSucceeded) {
        return true;
    }
    if (is_active_state(row.state)) {
        return true;
    }
    return row.progress_processed_bytes.has_value();
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
        for (const auto& source_id : map.value(QStringLiteral("sourceIds")).toList()) {
            row.source_ids.push_back(source_id.toString());
        }
        row.connection_id = map.value(QStringLiteral("connectionId")).toString();
        row.source_name = map.value(QStringLiteral("sourceName")).toString();
        row.destination_name = map.value(QStringLiteral("destinationName")).toString();
        row.destination_path = map.value(QStringLiteral("destinationPath")).toString();
        if (map.contains(QStringLiteral("requestedBackupType")) &&
            map.value(QStringLiteral("requestedBackupType")).isValid()) {
            row.requested_backup_type = map.value(QStringLiteral("requestedBackupType")).toLongLong();
        }
        if (map.contains(QStringLiteral("effectiveBackupType")) &&
            map.value(QStringLiteral("effectiveBackupType")).isValid()) {
            row.effective_backup_type = map.value(QStringLiteral("effectiveBackupType")).toLongLong();
        }
        row.effective_parent_uuid = map.value(QStringLiteral("effectiveParentUuid")).toString();
        if (map.contains(QStringLiteral("incrementalDowngradeReason")) &&
            map.value(QStringLiteral("incrementalDowngradeReason")).isValid()) {
            row.incremental_downgrade_reason =
                map.value(QStringLiteral("incrementalDowngradeReason")).toLongLong();
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace aegra::desktop
