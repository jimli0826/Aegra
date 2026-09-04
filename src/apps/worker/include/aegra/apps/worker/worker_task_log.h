#pragma once

#include "aegra/base/error.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace aegra::apps::worker {

/// Per-task file logger:
/// <data_dir>/logs/<operation>/YYYYMMDD_HHMMSS[_job-id].log
/// File-only; never writes secrets or credential material.
class WorkerTaskLog final {
  public:
    WorkerTaskLog(const WorkerTaskLog&) = delete;
    WorkerTaskLog& operator=(const WorkerTaskLog&) = delete;
    WorkerTaskLog(WorkerTaskLog&&) = delete;
    WorkerTaskLog& operator=(WorkerTaskLog&&) = delete;
    ~WorkerTaskLog();

    /// Opens a new timestamped log under logs/<operation>/.
    /// When job_id is non-empty and safe, it is appended to the file name.
    [[nodiscard]] static std::unique_ptr<WorkerTaskLog>
    open(std::string_view operation, std::string_view job_id = {}) noexcept;

    /// Thread-local active task log for nested runtime milestones (may be null).
    [[nodiscard]] static WorkerTaskLog* active() noexcept;

    void info(std::string_view message) noexcept;
    void warn(std::string_view message) noexcept;
    void error(std::string_view message) noexcept;

    /// Writes a blank line then `[title]`.
    void section(std::string_view title) noexcept;
    /// Writes `  key : value` (aligned for readability).
    void field(std::string_view key, std::string_view value) noexcept;
    void field_u64(std::string_view key, std::uint64_t value) noexcept;
    void field_bool(std::string_view key, bool value) noexcept;
    /// Writes `  key : 3.0 GiB (3203399680 bytes)`.
    void field_bytes(std::string_view key, std::uint64_t bytes) noexcept;

    void stage_begin(std::string_view stage) noexcept;
    void stage_ok(std::string_view stage, std::chrono::milliseconds elapsed) noexcept;
    void stage_fail(std::string_view stage, std::chrono::milliseconds elapsed,
                    const base::Error& error, std::string_view step = {},
                    std::string_view hint = {}) noexcept;

    [[nodiscard]] std::string_view path() const noexcept;

  private:
    friend class WorkerTaskLogScope;
    struct Impl;
    explicit WorkerTaskLog(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

/// Installs `log` as the thread-local active logger for the current scope.
class WorkerTaskLogScope final {
  public:
    explicit WorkerTaskLogScope(WorkerTaskLog* log) noexcept;
    ~WorkerTaskLogScope();
    WorkerTaskLogScope(const WorkerTaskLogScope&) = delete;
    WorkerTaskLogScope& operator=(const WorkerTaskLogScope&) = delete;

  private:
    WorkerTaskLog* previous_{nullptr};
};

/// RAII stage timer: logs stage_begin on construction; stage_ok on success path destructor
/// unless fail() was called.
class ScopedStage final {
  public:
    ScopedStage(WorkerTaskLog* log, std::string_view stage) noexcept;
    ~ScopedStage();
    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;

    void note(std::string_view key, std::string_view value) noexcept;
    void note_u64(std::string_view key, std::uint64_t value) noexcept;
    void note_bool(std::string_view key, bool value) noexcept;
    void note_bytes(std::string_view key, std::uint64_t bytes) noexcept;
    void fail(const base::Error& error, std::string_view step = {},
              std::string_view hint = {}) noexcept;
    [[nodiscard]] bool failed() const noexcept { return failed_; }

  private:
    WorkerTaskLog* log_{nullptr};
    std::string stage_;
    std::chrono::steady_clock::time_point started_{};
    bool finished_{false};
    bool failed_{false};
};

[[nodiscard]] std::string format_human_bytes(std::uint64_t bytes);
[[nodiscard]] std::string format_duration_ms(std::chrono::milliseconds elapsed);
[[nodiscard]] std::string path_display(const std::filesystem::path& path);

} // namespace aegra::apps::worker
