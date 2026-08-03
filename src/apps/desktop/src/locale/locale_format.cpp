#include "locale/locale_format.h"

#include <QDateTime>
#include <QTimeZone>
#include <QtGlobal>

#include <array>
#include <cmath>

namespace aegra::desktop {
namespace {

constexpr std::array<const char*, 5> kByteUnits{"B", "KiB", "MiB", "GiB", "TiB"};

} // namespace

LocaleFormat::LocaleFormat(QLocale locale) : locale_(std::move(locale)) {}

void LocaleFormat::set_locale(QLocale locale) { locale_ = std::move(locale); }

QString LocaleFormat::format_bytes(const std::int64_t bytes) const {
    if (bytes < 0) {
        return locale_.toString(0) + QLatin1Char(' ') + QLatin1String(kByteUnits[0]);
    }
    auto amount = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (amount >= 1024.0 && unit + 1U < kByteUnits.size()) {
        amount /= 1024.0;
        ++unit;
    }
    const auto precision = unit == 0U ? 0 : 1;
    return locale_.toString(amount, 'f', precision) + QLatin1Char(' ') +
           QLatin1String(kByteUnits[unit]);
}

QString LocaleFormat::format_date_time_utc_ms(const std::int64_t utc_ms) const {
    if (utc_ms <= 0) {
        //% "Not recorded"
        return qtTrId("aegra.common.not_recorded");
    }
    const auto date_time =
        QDateTime::fromMSecsSinceEpoch(utc_ms, QTimeZone::UTC).toLocalTime();
    return locale_.toString(date_time, QLocale::ShortFormat);
}

QString LocaleFormat::format_count(const int count) const { return locale_.toString(count); }

} // namespace aegra::desktop
