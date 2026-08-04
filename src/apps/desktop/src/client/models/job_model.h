#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVariantList>
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
};

class JobModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int runningCount READ runningCount NOTIFY countsChanged)
    Q_PROPERTY(int failedCount READ failedCount NOTIFY countsChanged)
    Q_PROPERTY(int succeededCount READ succeededCount NOTIFY countsChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY countsChanged)

  public:
    enum Role : int {
        JobIdRole = Qt::UserRole + 1,
        TraceIdRole,
        OperationTextRole,
        StateValueRole,
        StateTextRole,
        CreatedTextRole,
        ProgressPercentRole,
        ProgressVisibleRole,
        MessageTextRole,
        IsTerminalRole,
        IsActiveRole,
    };

    explicit JobModel(QObject* parent = nullptr);

    void set_locale_format(LocaleFormat* format);
    void set_rows(QVector<JobRow> rows);
    void clear();
    void retranslate();

    [[nodiscard]] int runningCount() const noexcept;
    [[nodiscard]] int failedCount() const noexcept;
    [[nodiscard]] int succeededCount() const noexcept;
    [[nodiscard]] int activeCount() const noexcept;
    [[nodiscard]] bool has_active_jobs() const noexcept;
    [[nodiscard]] std::optional<JobRow> find_job(const QString& job_id) const;

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  signals:
    void countChanged();
    void countsChanged();

  private:
    [[nodiscard]] QString operation_text(std::int64_t operation) const;
    [[nodiscard]] QString state_text(std::int64_t state) const;
    [[nodiscard]] static bool is_terminal_state(std::int64_t state) noexcept;
    [[nodiscard]] static bool is_active_state(std::int64_t state) noexcept;
    [[nodiscard]] static int progress_percent(const JobRow& row) noexcept;

    LocaleFormat* format_{nullptr};
    QVector<JobRow> rows_;
    int running_count_{0};
    int failed_count_{0};
    int succeeded_count_{0};
    int active_count_{0};
};

[[nodiscard]] QVector<JobRow> jobs_from_variant_list(const QVariantList& items);

} // namespace aegra::desktop
