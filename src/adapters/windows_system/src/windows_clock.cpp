#include "aegra/adapters/windows_system/windows_system.h"

#include <Windows.h>

#include <cstdint>

namespace aegra::adapters::windows_system {

std::int64_t WindowsSystemClock::now_utc_ms() const noexcept {
    constexpr std::uint64_t kWindowsToUnixEpochTicks = 116'444'736'000'000'000ULL;
    constexpr std::uint64_t kTicksPerMillisecond = 10'000ULL;
    FILETIME time{};
    GetSystemTimePreciseAsFileTime(&time);
    const auto ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) |
                       static_cast<std::uint64_t>(time.dwLowDateTime);
    if (ticks < kWindowsToUnixEpochTicks) {
        return -1;
    }
    return static_cast<std::int64_t>((ticks - kWindowsToUnixEpochTicks) / kTicksPerMillisecond);
}

} // namespace aegra::adapters::windows_system
