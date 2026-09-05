#include "aegra/apps/service/schedule_next_run.h"

#include <windows.h>

#include <chrono>
#include <optional>
#include <string>

namespace aegra::apps::service {
namespace {

using namespace std::chrono;

[[nodiscard]] bool month_day_is_selected(const std::uint32_t mask, const unsigned day) noexcept {
    return day >= 1 && day <= 31 && (mask & (1U << (day - 1U))) != 0;
}

// FILETIME epoch (1601-01-01) to Unix epoch (1970-01-01), in 100 ns ticks.
constexpr std::uint64_t kFiletimeToUnix100ns = 116444736000000000ULL;

[[nodiscard]] std::uint64_t filetime_to_unix_ms(const FILETIME ft) noexcept {
    const std::uint64_t ticks = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) |
                                static_cast<std::uint64_t>(ft.dwLowDateTime);
    if (ticks < kFiletimeToUnix100ns) {
        return 0;
    }
    return (ticks - kFiletimeToUnix100ns) / 10000ULL;
}

// The Service runs on the user's own machine, so the OS local time zone is the
// one the user picked minutes in. TzSpecificLocalTimeToSystemTime uses the live
// OS zone and is DST-aware for the given date — no ICU/tzdb dependency, always
// available in a Windows service. On any failure the wall time is treated as UTC.
[[nodiscard]] std::uint64_t local_wall_to_utc_ms(const year_month_day& ymd,
                                                 const std::uint16_t minute_of_day) noexcept {
    SYSTEMTIME local{};
    local.wYear = static_cast<WORD>(static_cast<int>(ymd.year()));
    local.wMonth = static_cast<WORD>(static_cast<unsigned>(ymd.month()));
    local.wDay = static_cast<WORD>(static_cast<unsigned>(ymd.day()));
    local.wHour = static_cast<WORD>(minute_of_day / 60U);
    local.wMinute = static_cast<WORD>(minute_of_day % 60U);
    SYSTEMTIME utc{};
    FILETIME ft{};
    if (TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) == 0 ||
        SystemTimeToFileTime(&utc, &ft) == 0) {
        if (SystemTimeToFileTime(&local, &ft) == 0) {
            return 0;
        }
    }
    return filetime_to_unix_ms(ft);
}

[[nodiscard]] year_month_day local_today(const std::uint64_t now_ms) noexcept {
    const std::uint64_t ticks = now_ms * 10000ULL + kFiletimeToUnix100ns;
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (FileTimeToSystemTime(&ft, &utc) == 0 ||
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) == 0) {
        const sys_seconds point{seconds{static_cast<std::int64_t>(now_ms / 1000ULL)}};
        return year_month_day{floor<days>(point)};
    }
    return year_month_day{year{local.wYear} / static_cast<unsigned>(local.wMonth) /
                          static_cast<unsigned>(local.wDay)};
}

void consider_candidate(std::optional<std::uint64_t>& best, const std::uint64_t candidate,
                        const std::uint64_t now_ms) noexcept {
    if (candidate <= now_ms) {
        return;
    }
    if (!best.has_value() || candidate < *best) {
        best = candidate;
    }
}

/// Next fire for daily: earliest local minute-of-day strictly after now.
/// Scans a few local days so a DST shift near midnight cannot skip the slot.
[[nodiscard]] std::uint64_t compute_daily_next_run(const contracts::ScheduleTrigger& trigger,
                                                   const std::uint64_t now_ms) {
    const sys_days today{local_today(now_ms)};
    std::optional<std::uint64_t> best;
    for (int day_offset = 0; day_offset <= 2; ++day_offset) {
        const year_month_day ymd{today + days{day_offset}};
        for (const auto minute : trigger.local_minutes_of_day) {
            consider_candidate(best, local_wall_to_utc_ms(ymd, minute), now_ms);
        }
    }
    return best.value_or(now_ms);
}

/// Next fire for weekly: advance local calendar days until weekday_mask matches.
[[nodiscard]] std::uint64_t compute_weekly_next_run(const contracts::ScheduleTrigger& trigger,
                                                    const std::uint64_t now_ms) {
    if (trigger.weekday_mask == 0) {
        return compute_daily_next_run(trigger, now_ms);
    }
    const sys_days today{local_today(now_ms)};
    std::optional<std::uint64_t> best;
    for (int day_offset = 0; day_offset <= 8; ++day_offset) {
        const sys_days day_point{today + days{day_offset}};
        const auto weekday_index =
            static_cast<std::uint8_t>(std::chrono::weekday{day_point}.c_encoding());
        if ((trigger.weekday_mask & static_cast<std::uint8_t>(1U << weekday_index)) == 0) {
            continue;
        }
        const year_month_day ymd{day_point};
        for (const auto minute : trigger.local_minutes_of_day) {
            consider_candidate(best, local_wall_to_utc_ms(ymd, minute), now_ms);
        }
    }
    return best.value_or(now_ms);
}

/// Next fire for monthly: natural local year_month_day; skip invalid days (e.g. Feb 31).
[[nodiscard]] std::uint64_t compute_monthly_next_run(const contracts::ScheduleTrigger& trigger,
                                                     const std::uint64_t now_ms) {
    if (trigger.day_of_month_mask == 0 || trigger.local_minutes_of_day.empty()) {
        return now_ms;
    }
    const year_month_day start = local_today(now_ms);
    const year_month cursor = start.year() / start.month();
    std::optional<std::uint64_t> best;
    // At most 24 months covers every selected day-of-month that exists in the calendar.
    for (int month_offset = 0; month_offset < 24 && !best.has_value(); ++month_offset) {
        const year_month ym = cursor + months{month_offset};
        for (unsigned day = 1; day <= 31; ++day) {
            if (!month_day_is_selected(trigger.day_of_month_mask, day)) {
                continue;
            }
            const year_month_day ymd{ym / day};
            if (!ymd.ok()) {
                continue;
            }
            for (const auto minute : trigger.local_minutes_of_day) {
                consider_candidate(best, local_wall_to_utc_ms(ymd, minute), now_ms);
            }
        }
    }
    return best.value_or(now_ms);
}

} // namespace

std::uint64_t compute_next_run_utc_ms(const contracts::ScheduleTrigger& trigger,
                                      const std::uint64_t now_ms) {
    switch (trigger.kind) {
    case contracts::ScheduleTriggerKind::kDaily:
        return compute_daily_next_run(trigger, now_ms);
    case contracts::ScheduleTriggerKind::kWeekly:
        return compute_weekly_next_run(trigger, now_ms);
    case contracts::ScheduleTriggerKind::kMonthly:
        return compute_monthly_next_run(trigger, now_ms);
    }
    return now_ms;
}

std::string make_schedule_fire_idempotency_key(const std::string_view schedule_id,
                                              const std::uint64_t due_next_run_utc_ms) {
    std::string key;
    key.reserve(schedule_id.size() + 32);
    key.append("schedule-fire|");
    key.append(schedule_id);
    key.push_back('|');
    key.append(std::to_string(due_next_run_utc_ms));
    return key;
}

} // namespace aegra::apps::service
