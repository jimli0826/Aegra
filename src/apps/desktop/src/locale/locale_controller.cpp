#include "locale/locale_controller.h"

#include <QCoreApplication>
#include <QFile>
#include <QQmlEngine>
#include <QSettings>
#include <QTranslator>
#include <QVariantMap>

namespace aegra::desktop {
namespace {

constexpr auto kSettingsKey = "ui/language";
constexpr auto kThemeSettingsKey = "ui/theme";
constexpr auto kTranslationPrefix = ":/Aegra/i18n/aegra_desktop_";

[[nodiscard]] QLocale locale_for_tag(const QString& language_tag) {
    for (const auto& entry :
         std::initializer_list<std::pair<const char*, QLocale>>{
             {"en_US", QLocale(QLocale::English, QLocale::UnitedStates)},
             {"zh_CN", QLocale(QLocale::Chinese, QLocale::China)},
             {"zh_TW", QLocale(QLocale::Chinese, QLocale::Taiwan)},
             {"ja_JP", QLocale(QLocale::Japanese, QLocale::Japan)},
             {"de_DE", QLocale(QLocale::German, QLocale::Germany)},
         }) {
        if (language_tag == QLatin1String(entry.first)) {
            return entry.second;
        }
    }
    return QLocale(QLocale::English, QLocale::UnitedStates);
}

} // namespace

LocaleController::LocaleController(QQmlEngine* engine, QObject* parent)
    : QObject(parent), engine_(engine), translator_(new QTranslator(this)),
      language_tag_(QStringLiteral("en_US")), theme_id_(QStringLiteral("blueExtra")),
      locale_(QLocale::English, QLocale::UnitedStates) {
    load_saved_or_system_language();
    load_saved_theme();
}

LocaleController::~LocaleController() = default;

QString LocaleController::language() const { return language_tag_; }

QString LocaleController::theme() const { return theme_id_; }

QString LocaleController::languageLabel() const {
    for (const auto& entry : supported_languages()) {
        if (entry.tag == language_tag_) {
            return entry.native_label;
        }
    }
    return language_tag_;
}

QVariantList LocaleController::availableLanguages() const {
    QVariantList result;
    for (const auto& entry : supported_languages()) {
        result.push_back(QVariantMap{{QStringLiteral("tag"), entry.tag},
                                     {QStringLiteral("label"), entry.native_label}});
    }
    return result;
}

QLocale LocaleController::locale() const { return locale_; }

bool LocaleController::setLanguage(const QString& language_tag) {
    const auto matched = match_supported(language_tag);
    if (matched.isEmpty()) {
        return false;
    }
    return apply_language(matched, true);
}

void LocaleController::load_saved_or_system_language() {
    QSettings settings;
    const auto saved = settings.value(QLatin1String(kSettingsKey)).toString();
    if (!saved.isEmpty()) {
        const auto matched = match_supported(saved);
        if (!matched.isEmpty() && apply_language(matched, false)) {
            return;
        }
    }
    const auto system = match_supported(system_language_tag());
    if (!system.isEmpty() && apply_language(system, false)) {
        return;
    }
    if (!apply_language(QStringLiteral("en_US"), false)) {
        language_tag_ = QStringLiteral("en_US");
        locale_ = QLocale(QLocale::English, QLocale::UnitedStates);
    }
}

QList<LocaleController::LanguageEntry> LocaleController::supported_languages() {
    return {
        {QStringLiteral("en_US"), QStringLiteral("English"),
         QLocale(QLocale::English, QLocale::UnitedStates)},
        {QStringLiteral("zh_CN"), QStringLiteral("简体中文"),
         QLocale(QLocale::Chinese, QLocale::China)},
        {QStringLiteral("zh_TW"), QStringLiteral("繁體中文"),
         QLocale(QLocale::Chinese, QLocale::Taiwan)},
        {QStringLiteral("ja_JP"), QStringLiteral("日本語"),
         QLocale(QLocale::Japanese, QLocale::Japan)},
        {QStringLiteral("de_DE"), QStringLiteral("Deutsch"),
         QLocale(QLocale::German, QLocale::Germany)},
    };
}

QString LocaleController::normalize_tag(const QString& tag) {
    auto normalized = tag.trimmed();
    normalized.replace(QLatin1Char('-'), QLatin1Char('_'));
    return normalized;
}

QString LocaleController::match_supported(const QString& candidate) {
    const auto normalized = normalize_tag(candidate);
    if (normalized.isEmpty()) {
        return {};
    }
    for (const auto& entry : supported_languages()) {
        if (entry.tag.compare(normalized, Qt::CaseInsensitive) == 0) {
            return entry.tag;
        }
    }
    if (normalized.compare(QLatin1String("en"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("en_US");
    }
    if (normalized.compare(QLatin1String("ja"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("ja_JP");
    }
    if (normalized.compare(QLatin1String("de"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("de_DE");
    }
    if (normalized.compare(QLatin1String("zh"), Qt::CaseInsensitive) == 0 ||
        normalized.compare(QLatin1String("zh_Hans"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("zh_CN");
    }
    if (normalized.compare(QLatin1String("zh_Hant"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("zh_TW");
    }
    const auto locale = QLocale(normalized);
    for (const auto& entry : supported_languages()) {
        if (entry.locale.language() == locale.language() &&
            (locale.territory() == QLocale::AnyTerritory ||
             entry.locale.territory() == locale.territory())) {
            return entry.tag;
        }
    }
    return {};
}

QString LocaleController::system_language_tag() { return QLocale::system().name(); }

bool LocaleController::apply_language(const QString& language_tag, const bool persist) {
    auto applied = language_tag;
    if (!install_translator(language_tag)) {
        // Missing or corrupt QM must not block startup; fall back to English source text.
        if (!install_translator(QStringLiteral("en_US"))) {
            language_tag_ = QStringLiteral("en_US");
            locale_ = QLocale(QLocale::English, QLocale::UnitedStates);
            if (persist) {
                save_language(language_tag_);
            }
            if (engine_ != nullptr) {
                engine_->retranslate();
            }
            emit languageChanged();
            return false;
        }
        applied = QStringLiteral("en_US");
    }

    language_tag_ = applied;
    locale_ = locale_for_tag(applied);
    if (persist) {
        save_language(language_tag_);
    }
    if (engine_ != nullptr) {
        engine_->retranslate();
    }
    emit languageChanged();
    return applied == language_tag;
}

bool LocaleController::install_translator(const QString& language_tag) {
    QCoreApplication::removeTranslator(translator_);
    const auto path = QLatin1String(kTranslationPrefix) + language_tag + QLatin1String(".qm");
    if (!QFile::exists(path)) {
        // Missing QM is non-fatal for en_US source fallback via //% / ID text.
        return language_tag == QLatin1String("en_US");
    }
    if (!translator_->load(path)) {
        return language_tag == QLatin1String("en_US");
    }
    return QCoreApplication::installTranslator(translator_);
}

void LocaleController::save_language(const QString& language_tag) const {
    QSettings settings;
    settings.setValue(QLatin1String(kSettingsKey), language_tag);
}

bool LocaleController::is_supported_theme(const QString& theme_id) {
    return theme_id == QLatin1String("blueExtra") || theme_id == QLatin1String("oceanBlue") ||
           theme_id == QLatin1String("dark");
}

void LocaleController::load_saved_theme() {
    QSettings settings;
    const auto id =
        settings.value(QLatin1String(kThemeSettingsKey), QStringLiteral("blueExtra")).toString();
    theme_id_ = is_supported_theme(id) ? id : QStringLiteral("blueExtra");
}

void LocaleController::save_theme() const {
    QSettings settings;
    settings.setValue(QLatin1String(kThemeSettingsKey), theme_id_);
}

void LocaleController::setTheme(const QString& theme_id) {
    if (!is_supported_theme(theme_id) || theme_id_ == theme_id) {
        return;
    }
    theme_id_ = theme_id;
    save_theme();
    emit themeChanged();
}

} // namespace aegra::desktop
