#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace aegra::apps::service {

/// Shared mutex between ScheduleService mutations and ScheduleEngine due-fire.
/// Injected from the Service composition root — not a singleton.
class ScheduleExecutionCoordinator final {
  public:
    ScheduleExecutionCoordinator() = default;
    ScheduleExecutionCoordinator(const ScheduleExecutionCoordinator&) = delete;
    ScheduleExecutionCoordinator& operator=(const ScheduleExecutionCoordinator&) = delete;

    class Lock final {
      public:
        explicit Lock(std::unique_lock<std::timed_mutex> lock) noexcept : lock_(std::move(lock)) {}
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
        Lock(Lock&&) noexcept = default;
        Lock& operator=(Lock&&) noexcept = default;

      private:
        std::unique_lock<std::timed_mutex> lock_;
    };

    /// Waits in bounded intervals so request deadlines and Service stop can cancel lock wait.
    [[nodiscard]] base::Result<Lock> acquire(base::CancellationToken cancellation) {
        using namespace std::chrono_literals;
        if (cancellation.stop_requested()) {
            return base::Result<Lock>::failure(
                {base::ErrorCode::kCancelled, "schedule coordination was cancelled"});
        }
        std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
        while (!lock.try_lock_for(100ms)) {
            if (cancellation.stop_requested()) {
                return base::Result<Lock>::failure(
                    {base::ErrorCode::kCancelled, "schedule coordination was cancelled"});
            }
        }
        return base::Result<Lock>::success(Lock(std::move(lock)));
    }

  private:
    std::timed_mutex mutex_;
};

/// Outcome of an engine-driven scheduled backup attempt.
enum class ScheduledBackupOutcome : std::uint8_t {
    kAccepted = 1,
    kReplayed = 2,
    /// Schedule disabled, deleted, due slot changed, or not yet due.
    kSkipped = 3,
    /// Worker capacity full; keep the due slot and retry later.
    kDeferredCapacity = 4,
};

struct ScheduledBackupResult final {
    ScheduledBackupOutcome outcome{ScheduledBackupOutcome::kSkipped};
    std::optional<std::string> job_id;
    std::string detail;
};

} // namespace aegra::apps::service
