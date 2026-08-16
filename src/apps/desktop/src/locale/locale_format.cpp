#include "locale/locale_format.h"

#include <QDateTime>
#include <QTimeZone>
#include <QtGlobal>

#include <array>
#include <cmath>

namespace aegra::desktop {
namespace {

// Match old StorageManager::FormatSize labels (binary steps, decimal unit names).
constexpr std::array<const char*, 5> kByteUnits{"B", "KB", "MB", "GB", "TB"};

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
    // B: integer; KB/MB: 1 decimal; GB/TB: 2 decimals (e.g. "7.03 GB").
    const auto precision = unit == 0U ? 0 : (unit >= 3U ? 2 : 1);
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

QString localized_volume_label(const QString& label) {
    const auto trimmed = label.trimmed();
    if (trimmed.isEmpty() ||
        trimmed.compare(QStringLiteral("Local Disk"), Qt::CaseInsensitive) == 0 ||
        trimmed == QStringLiteral("本地磁盘") || trimmed == QStringLiteral("本機磁碟")) {
        //% "Local Disk"
        return qtTrId("aegra.volume.local_disk");
    }
    // Chinese Windows default label is 新加卷 / 新建卷; English is "New Volume".
    if (trimmed.compare(QStringLiteral("New Volume"), Qt::CaseInsensitive) == 0 ||
        trimmed == QStringLiteral("新加卷") || trimmed == QStringLiteral("新建卷") ||
        trimmed == QStringLiteral("新加捲")) {
        //% "New Volume"
        return qtTrId("aegra.volume.new_volume");
    }
    if (trimmed.compare(QStringLiteral("Hidden Partition"), Qt::CaseInsensitive) == 0) {
        //% "Hidden Partition"
        return qtTrId("aegra.volume.hidden_partition");
    }
    if (trimmed.compare(QStringLiteral("EFI System Partition"), Qt::CaseInsensitive) == 0) {
        //% "EFI System Partition"
        return qtTrId("aegra.volume.efi_system");
    }
    if (trimmed.compare(QStringLiteral("Recovery Partition"), Qt::CaseInsensitive) == 0) {
        //% "Recovery Partition"
        return qtTrId("aegra.volume.recovery");
    }
    if (trimmed.compare(QStringLiteral("System"), Qt::CaseInsensitive) == 0) {
        //% "System"
        return qtTrId("aegra.volume.system");
    }
    return trimmed;
}

QString volume_display_title(const QString& label, const QString& mount_letter) {
    auto letter = mount_letter.trimmed();
    if (letter.size() == 1) {
        letter += QLatin1Char(':');
    }
    const auto name = localized_volume_label(label);
    if (!letter.isEmpty() && !name.isEmpty()) {
        return name + QStringLiteral(" (") + letter + QLatin1Char(')');
    }
    if (!letter.isEmpty()) {
        return letter;
    }
    return name;
}

} // namespace aegra::desktop
