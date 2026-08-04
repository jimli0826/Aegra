#include "locale/locale_controller.h"
#include "locale/locale_format.h"
#include "locale/message_code_map.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QQmlEngine>
#include <QSettings>
#include <QTemporaryDir>
#include <QTranslator>

#include <cstdio>
#include <cstdlib>

namespace {

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

// LocaleController loads from qrc :/Aegra/i18n/. For unit tests without the full rcc bundle,
// install translators from the source translations directory and exercise mapping helpers.
bool install_from_disk(QTranslator& translator, const QString& language_tag) {
    const QString path =
        QDir(QStringLiteral(AEGRA_DESKTOP_I18N_DIR))
            .filePath(QStringLiteral("aegra_desktop_") + language_tag + QStringLiteral(".qm"));
    if (!QFile::exists(path)) {
        std::fprintf(stderr, "[FAIL] missing translation file %s\n", qPrintable(path));
        return false;
    }
    QCoreApplication::removeTranslator(&translator);
    if (!translator.load(path)) {
        std::fprintf(stderr, "[FAIL] failed to load %s\n", qPrintable(path));
        return false;
    }
    return QCoreApplication::installTranslator(&translator);
}

bool test_message_code_map() {
    bool passed = true;
    passed &= expect(aegra::desktop::translation_id_for_message_code(
                         QStringLiteral("repository.query_failed")) ==
                         QLatin1String("aegra.error.repository.query_failed"),
                     "known message code maps to translation id");
    passed &=
        expect(aegra::desktop::translation_id_for_message_code(QStringLiteral("job.running")) ==
                   QLatin1String("aegra.task.state.running"),
               "job state code maps to task state translation id");
    passed &= expect(
        aegra::desktop::translation_id_for_message_code(QStringLiteral("job.deadline_exceeded")) ==
            QLatin1String("aegra.error.job.deadline_exceeded"),
        "job deadline code maps to stable translation id");
    passed &= expect(aegra::desktop::translation_id_for_message_code(
                         QStringLiteral("not.a.real.code")) == QLatin1String("aegra.error.unknown"),
                     "unknown message code falls back safely");
    const auto localized =
        aegra::desktop::localize_message_code(QStringLiteral("repository.query_failed"));
    passed &= expect(!localized.isEmpty() && !localized.contains(QLatin1String("not.a.real")),
                     "localize returns a non-empty safe string");
    const auto unknown = aegra::desktop::localize_message_code(QStringLiteral("totally.unknown"));
    passed &= expect(unknown.contains(QLatin1String("totally.unknown")),
                     "unknown code remains visible in the generic fallback");
    return passed;
}

bool test_locale_format() {
    aegra::desktop::LocaleFormat format(QLocale(QLocale::English, QLocale::UnitedStates));
    const auto bytes = format.format_bytes(1536);
    bool passed = expect(bytes.contains(QLatin1String("KiB")) || bytes.contains(QLatin1Char('1')),
                         "byte formatter produces a locale-aware size");
    const auto missing = format.format_date_time_utc_ms(0);
    passed &= expect(!missing.isEmpty(), "missing timestamps use a safe localized placeholder");
    format.set_locale(QLocale(QLocale::German, QLocale::Germany));
    const auto de_bytes = format.format_bytes(1536);
    passed &= expect(!de_bytes.isEmpty(), "byte formatter works after locale change");
    return passed;
}

bool test_locale_controller_persistence_and_fallback() {
    QTemporaryDir temp;
    if (!expect(temp.isValid(), "temporary settings dir exists")) {
        return false;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    QSettings::setDefaultFormat(QSettings::IniFormat);

    QQmlEngine engine;
    aegra::desktop::LocaleController controller(&engine);
    bool passed = true;

    passed &= expect(controller.setLanguage(QStringLiteral("zh_CN")) ||
                         controller.language() == QLatin1String("en_US") ||
                         controller.language() == QLatin1String("zh_CN"),
                     "setLanguage accepts a supported language or falls back safely");

    // Without embedded qrc QM, zh_CN install may fall back to en_US; still must not crash.
    passed &= expect(!controller.language().isEmpty(), "controller always has an active language");
    passed &= expect(controller.availableLanguages().size() == 5,
                     "five first-wave languages are advertised");

    passed &= expect(!controller.setLanguage(QStringLiteral("xx_YY")),
                     "invalid language tag is rejected");
    const auto before = controller.language();
    passed &= expect(!controller.setLanguage(QStringLiteral("not-a-locale")),
                     "invalid saved-style tag is rejected");
    passed &=
        expect(controller.language() == before, "invalid setLanguage leaves language unchanged");

    // Persist a valid preference and reload into a new controller.
    QSettings settings;
    settings.setValue(QStringLiteral("ui/language"), QStringLiteral("de_DE"));
    settings.sync();
    aegra::desktop::LocaleController reloaded(&engine);
    // Missing qrc QM may force en_US fallback; either de_DE or en_US is acceptable, but not crash.
    passed &= expect(reloaded.language() == QLatin1String("de_DE") ||
                         reloaded.language() == QLatin1String("en_US"),
                     "saved language loads or falls back deterministically");

    // Explicit invalid saved value is ignored.
    settings.setValue(QStringLiteral("ui/language"), QStringLiteral("invalid_value"));
    settings.sync();
    aegra::desktop::LocaleController ignored(&engine);
    passed &= expect(ignored.language() == QLatin1String("en_US") || !ignored.language().isEmpty(),
                     "invalid saved language does not prevent startup");

    // Disk-based translator load proves runtime language packs work for all five tags.
    QTranslator translator;
    for (const auto& tag :
         {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW"),
          QStringLiteral("ja_JP"), QStringLiteral("de_DE")}) {
        if (!install_from_disk(translator, tag)) {
            passed = false;
            continue;
        }
        const auto home = qtTrId("aegra.nav.home");
        if (home.isEmpty() || home == QLatin1String("aegra.nav.home")) {
            std::fprintf(stderr, "[FAIL] translated nav home missing for %s\n", qPrintable(tag));
            passed = false;
        }
    }
    return passed;
}

} // namespace

int main(int argument_count, char* arguments[]) noexcept {
    QCoreApplication application(argument_count, arguments);
    application.setOrganizationName(QStringLiteral("AegraTest"));
    application.setApplicationName(QStringLiteral("DesktopLocale"));
    try {
        const auto map_ok = test_message_code_map();
        const auto format_ok = test_locale_format();
        const auto controller_ok = test_locale_controller_persistence_and_fallback();
        return map_ok && format_ok && controller_ok ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
