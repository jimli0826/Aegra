#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <cstdint>
#include <optional>

namespace aegra::desktop {

class LocaleFormat;

struct JobRow final {
    QString job_id;
    QString trace_id;
    std::int64_t operation{0};
    std::int64_t state{0};
    std::int64_t created_utc_ms{0};
    std::optional<std::int64_t> started_utc_ms;
    std::optional<std::int64_t> completed_utc_ms;
    std::optional<std::int64_t> progress_phase;
    std::optional<std::int64_t> progress_logical_bytes;
    std::optional<std::int64_t> progress_processed_bytes;
    std::optional<std::int64_t> progress_stored_bytes;
    QString message_code;
    QStringList source_ids;
    /// Owning schedule for backup jobs; empty for other operations.
    QString schedule_id;
    QString connection_id;
    QString source_name;
    QString destination_name;
    QString destination_path;
    /// file_set backup: Service-projected requested vs effective type (1 full, 2 inc).
    std::optional<std::int64_t> requested_backup_type;
    std::optional<std::int64_t> effective_backup_type;
    QString effective_parent_uuid;
    /// contracts::IncrementalDowngradeReason when Incremental demoted to Full (1, 2, 3, or 9).
    std::optional<std::int64_t> incremental_downgrade_reason;
};

class JobModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int runningCount READ runningCount NOTIFY countsChanged)
    Q_PROPERTY(int failedCount READ failedCount NOTIFY countsChanged)
    Q_PROPERTY(int succeededCount READ succeededCount NOTIFY countsChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY countsChanged)
    /// Bumps when job rows or progress change so QML bindings can re-query status.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

  public:
    enum Role : int {
        JobIdRole = Qt::UserRole + 1,
        TraceIdRole,
        OperationTextRole,
        StateValueRole,
        StateTextRole,
        StateColorRole,
        CreatedTextRole,
        ProgressPercentRole,
        ProgressVisibleRole,
        MessageTextRole,
        IsTerminalRole,
        IsActiveRole,
        SourceNameRole,
        DestinationNameRole,
        DestinationPathRole,
        SourceIdsRole,
        ConnectionIdRole,
        RequestedBackupTypeTextRole,
        EffectiveBackupTypeTextRole,
        DowngradeReasonTextRole,
        HasDowngradeRole,
    };

    explicit JobModel(QObject* parent = nullptr);

    void set_locale_format(LocaleFormat* format);
    void set_rows(QVector<JobRow> rows);
    /// Insert or replace one job without dropping the rest (optimistic Run feedback).
    void upsert_job(JobRow row);
    void clear();
    void retranslate();

    [[nodiscard]] int runningCount() const noexcept;
    [[nodiscard]] int failedCount() const noexcept;
    [[nodiscard]] int succeededCount() const noexcept;
    [[nodiscard]] int activeCount() const noexcept;
    [[nodiscard]] int revision() const noexcept;
    [[nodiscard]] bool has_active_jobs() const noexcept;
    [[nodiscard]] std::optional<JobRow> find_job(const QString& job_id) const;

    /// Latest backup job for a schedule (matched by schedule_id).
    /// Keys: statusKey (none|running|success|failed), progressPercent, stateText, stateValue.
    Q_INVOKABLE [[nodiscard]] QVariantMap latestBackupStatus(const QString& schedule_id) const;

    /// Aggregate restore jobs (operation=2) created at/after sinceUtcMs (0 = all restore jobs).
    /// Keys: jobCount, activeCount, progressPercent, stateText, messageText, sourceName,
    /// statusKey (none|running|success|failed), allTerminal, anyFailed.
    Q_INVOKABLE [[nodiscard]] QVariantMap restoreSessionStatus(qint64 since_utc_ms = 0) const;

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  signals:
    void countChanged();
    void countsChanged();
    void revisionChanged();

  private:
    [[nodiscard]] QString operation_text(std::int64_t operation) const;
    [[nodiscard]] QString state_text(std::int64_t state) const;
    [[nodiscard]] QString backup_type_text(std::int64_t backup_type) const;
    [[nodiscard]] QString downgrade_reason_text(std::int64_t reason) const;
    [[nodiscard]] static QString state_color(std::int64_t state) noexcept;
    [[nodiscard]] static bool is_terminal_state(std::int64_t state) noexcept;
    [[nodiscard]] static bool is_active_state(std::int64_t state) noexcept;
    [[nodiscard]] static int progress_percent(const JobRow& row) noexcept;
    [[nodiscard]] static bool progress_visible(const JobRow& row) noexcept;
    void bump_revision();

    LocaleFormat* format_{nullptr};
    QVector<JobRow> rows_;
    int running_count_{0};
    int failed_count_{0};
    int succeeded_count_{0};
    int active_count_{0};
    int revision_{0};
};

[[nodiscard]] QVector<JobRow> jobs_from_variant_list(const QVariantList& items);

} // namespace aegra::desktop
