#include "aegra/apps/service/schedule_next_run.h"

#include <chrono>
#include <optional>
#include <string>

namespace aegra::apps::service {
namespace {

using namespace std::chrono;

constexpr std::uint64_t kDayMs = 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr std::uint64_t kMinuteMs = 60ULL * 1000ULL;

[[nodiscard]] bool month_day_is_selected(const std::uint32_t mask, const unsigned day) noexcept {
    return day >= 1 && day <= 31 && (mask & (1U << (day - 1U))) != 0;
}

[[nodiscard]] std::uint64_t utc_ms_from_sys_days(const sys_days day,
                                                 const std::uint16_t minute_of_day) noexcept {
    const auto day_ms =
        static_cast<std::uint64_t>(duration_cast<milliseconds>(day.time_since_epoch()).count());
    return day_ms + static_cast<std::uint64_t>(minute_of_day) * kMinuteMs;
}

[[nodiscard]] year_month_day utc_ymd(const std::uint64_t now_ms) {
    const sys_seconds time_point{seconds{static_cast<std::int64_t>(now_ms / 1000ULL)}};
    return year_month_day{floor<days>(time_point)};
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

/// Next fire for daily: earliest minute-of-day strictly after now (rolling days as needed).
[[nodiscard]] std::uint64_t compute_daily_next_run(const contracts::ScheduleTrigger& trigger,
                                                   const std::uint64_t now_ms) {
    std::optional<std::uint64_t> best;
    const auto day_start = (now_ms / kDayMs) * kDayMs;
    for (const auto minute : trigger.local_minutes_of_day) {
        auto candidate = day_start + static_cast<std::uint64_t>(minute) * kMinuteMs;
        if (candidate <= now_ms) {
            candidate += kDayMs;
        }
        consider_candidate(best, candidate, now_ms);
    }
    return best.value_or(now_ms);
}

/// Next fire for weekly: advance calendar days until weekday_mask matches.
[[nodiscard]] std::uint64_t compute_weekly_next_run(const contracts::ScheduleTrigger& trigger,
                                                    const std::uint64_t now_ms) {
    std::optional<std::uint64_t> best;
    if (trigger.weekday_mask == 0) {
        return compute_daily_next_run(trigger, now_ms);
    }
    const auto day_start = (now_ms / kDayMs) * kDayMs;
    for (const auto minute : trigger.local_minutes_of_day) {
        auto candidate = day_start + static_cast<std::uint64_t>(minute) * kMinuteMs;
        if (candidate <= now_ms) {
            candidate += kDayMs;
        }
        for (int step = 0; step < 8; ++step) {
            const auto days_since_epoch = candidate / kDayMs;
            const auto weekday = static_cast<std::uint8_t>((days_since_epoch + 4U) % 7U);
            if ((trigger.weekday_mask & static_cast<std::uint8_t>(1U << weekday)) != 0) {
                consider_candidate(best, candidate, now_ms);
                break;
            }
            candidate += kDayMs;
        }
    }
    return best.value_or(now_ms);
}

/// Next fire for monthly: natural year_month_day; skip invalid days (e.g. Feb 31).
[[nodiscard]] std::uint64_t compute_monthly_next_run(const contracts::ScheduleTrigger& trigger,
                                                     const std::uint64_t now_ms) {
    std::optional<std::uint64_t> best;
    if (trigger.day_of_month_mask == 0 || trigger.local_minutes_of_day.empty()) {
        return now_ms;
    }
    const year_month_day start = utc_ymd(now_ms);
    year_month cursor = start.year() / start.month();
    // At most 24 months covers every selected day-of-month that exists in the calendar.
    for (int month_offset = 0; month_offset < 24; ++month_offset) {
        const year_month ym = cursor + months{month_offset};
        std::optional<std::uint64_t> month_best;
        for (unsigned day = 1; day <= 31; ++day) {
            if (!month_day_is_selected(trigger.day_of_month_mask, day)) {
                continue;
            }
            const year_month_day ymd{ym / day};
            if (!ymd.ok()) {
                continue;
            }
            const sys_days day_point{ymd};
            for (const auto minute : trigger.local_minutes_of_day) {
                const auto candidate = utc_ms_from_sys_days(day_point, minute);
                consider_candidate(month_best, candidate, now_ms);
            }
        }
        if (month_best.has_value()) {
            best = month_best;
            // Later months are always later for the same day-of-month / minute grid.
            break;
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
