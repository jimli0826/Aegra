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

  public:
    explicit LocaleController(QQmlEngine* engine, QObject* parent = nullptr);
    ~LocaleController() override;

    [[nodiscard]] QString language() const;
    [[nodiscard]] QString languageLabel() const;
    [[nodiscard]] QVariantList availableLanguages() const;
    [[nodiscard]] QLocale locale() const;

    // Applies a BCP-47 style language tag such as "en_US" or "zh_CN". Invalid values are
    // ignored and the previous language remains active.
    Q_INVOKABLE bool setLanguage(const QString& language_tag);

    // Reloads preference from QSettings (tests may call after writing settings).
    void load_saved_or_system_language();

  signals:
    void languageChanged();

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

    QQmlEngine* engine_{nullptr};
    QTranslator* translator_{nullptr};
    QString language_tag_;
    QLocale locale_;
};

} // namespace aegra::desktop
