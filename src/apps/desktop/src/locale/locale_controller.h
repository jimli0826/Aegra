#pragma once

#include <QLocale>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class QQmlEngine;
class QTranslator;

namespace aegra::desktop {

// Owns Desktop UI language preference and QTranslator lifecycle. Constructed in the
// composition root and injected into QML; no global mutable language singleton.
class LocaleController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(QString languageLabel READ languageLabel NOTIFY languageChanged)
    Q_PROPERTY(QVariantList availableLanguages READ availableLanguages CONSTANT)
    // Desktop theme id: blueExtra (glass default) | dark | light.
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)

  public:
    explicit LocaleController(QQmlEngine* engine, QObject* parent = nullptr);
    ~LocaleController() override;

    [[nodiscard]] QString language() const;
    [[nodiscard]] QString languageLabel() const;
    [[nodiscard]] QVariantList availableLanguages() const;
    [[nodiscard]] QLocale locale() const;
    [[nodiscard]] QString theme() const;

    // Applies a BCP-47 style language tag such as "en_US" or "zh_CN". Invalid values are
    // ignored and the previous language remains active.
    Q_INVOKABLE bool setLanguage(const QString& language_tag);
    // Applies a theme id and persists to QSettings ui/theme (old SystemBackend pattern).
    Q_INVOKABLE void setTheme(const QString& theme_id);

    // Reloads preference from QSettings (tests may call after writing settings).
    void load_saved_or_system_language();
    void load_saved_theme();

  signals:
    void languageChanged();
    void themeChanged();

  private:
    struct LanguageEntry final {
        QString tag;
        QString native_label;
        QLocale locale;
    };

    [[nodiscard]] static QList<LanguageEntry> supported_languages();
    [[nodiscard]] static QString normalize_tag(const QString& tag);
    [[nodiscard]] static QString match_supported(const QString& candidate);
    [[nodiscard]] static QString system_language_tag();
    [[nodiscard]] bool apply_language(const QString& language_tag, bool persist);
    [[nodiscard]] bool install_translator(const QString& language_tag);
    void save_language(const QString& language_tag) const;
    void save_theme() const;
    [[nodiscard]] static bool is_supported_theme(const QString& theme_id);

    QQmlEngine* engine_{nullptr};
    QTranslator* translator_{nullptr};
    QString language_tag_;
    QString theme_id_{QStringLiteral("blueExtra")};
    QLocale locale_;
};

} // namespace aegra::desktop
