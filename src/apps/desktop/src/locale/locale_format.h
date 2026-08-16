#pragma once

#include <QLocale>
#include <QString>

#include <cstdint>

namespace aegra::desktop {

// Locale-aware display helpers for QML and models. Uses the supplied QLocale; does not
// read global mutable language state.
class LocaleFormat final {
  public:
    explicit LocaleFormat(QLocale locale = QLocale{});

    void set_locale(QLocale locale);

    [[nodiscard]] QString format_bytes(std::int64_t bytes) const;
    [[nodiscard]] QString format_date_time_utc_ms(std::int64_t utc_ms) const;
    [[nodiscard]] QString format_count(int count) const;

  private:
    QLocale locale_;
};

/// Map Service/Windows English stock labels to the active Desktop language
/// (e.g. "Local Disk" → 本地磁盘, "New Volume" → 新加卷). Custom labels pass through.
[[nodiscard]] QString localized_volume_label(const QString& label);

/// Partition-bar / tree title: "本地磁盘 (C:)" when both name and letter exist.
[[nodiscard]] QString volume_display_title(const QString& label, const QString& mount_letter);

} // namespace aegra::desktop
