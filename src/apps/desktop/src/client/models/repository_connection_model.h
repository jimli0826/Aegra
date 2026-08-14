#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

#include <cstdint>
#include <optional>

namespace aegra::desktop {

struct RepositoryConnectionRow final {
    QString connection_id;
    QString display_name;
    QString locator;
    std::int64_t state{2};
    bool is_default{false};
    bool refreshing{false};
    /// Service message_code for last Test/Refresh probe failure (localized in data()).
    QString probe_error_code;
    /// Free bytes from last Online Test free-space probe; nullopt = unknown / not probed.
    std::optional<qint64> free_bytes;
    QStringList capabilities;
};

// Domain list model for Repository connections returned by Service V3.
class RepositoryConnectionModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int availableCount READ availableCount NOTIFY countChanged)

  public:
    enum Role : int {
        ConnectionIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        LocatorRole,
        StateValueRole,
        StateTextRole,
        IsDefaultRole,
        IsAvailableRole,
        IsRefreshingRole,
        ProbeErrorTextRole,
        FreeBytesRole,
        HasFreeBytesRole,
        CapabilitiesRole,
    };

    explicit RepositoryConnectionModel(QObject* parent = nullptr);

    void set_rows(QVector<RepositoryConnectionRow> rows);
    void clear();
    void retranslate();

    [[nodiscard]] int availableCount() const noexcept;
    [[nodiscard]] bool contains_available(const QString& connection_id) const;
    [[nodiscard]] std::optional<RepositoryConnectionRow> find(const QString& connection_id) const;
    [[nodiscard]] QString default_connection_id() const;
    [[nodiscard]] QStringList connection_ids() const;
    bool set_refreshing(const QString& connection_id, bool refreshing);
    void clear_refreshing();
    bool set_probe_error(const QString& connection_id, const QString& message_code);
    void clear_probe_error(const QString& connection_id);
    bool set_free_bytes(const QString& connection_id, std::optional<qint64> free_bytes);
    [[nodiscard]] std::optional<qint64> free_bytes_for_locator(const QString& locator) const;

    /// QML helpers for wizard destination selection by list index.
    Q_INVOKABLE [[nodiscard]] QString connectionIdAt(int row) const;
    Q_INVOKABLE [[nodiscard]] bool isAvailableAt(int row) const;
    Q_INVOKABLE [[nodiscard]] int indexOfConnectionId(const QString& connection_id) const;

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  signals:
    void countChanged();

  private:
    [[nodiscard]] static bool is_available_state(std::int64_t state) noexcept;
    [[nodiscard]] QString state_text(const RepositoryConnectionRow& row) const;

    QVector<RepositoryConnectionRow> rows_;
    int available_count_{0};
};

[[nodiscard]] QVector<RepositoryConnectionRow>
connections_from_variant_list(const QVariantList& items);

} // namespace aegra::desktop
