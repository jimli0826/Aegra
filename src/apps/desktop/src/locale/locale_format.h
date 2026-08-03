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

} // namespace aegra::desktop
