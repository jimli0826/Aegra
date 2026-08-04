#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace aegra::apps::worker {

/// Per-task file logger (old AegraImage style):
/// <data_dir>/logs/<operation>/YYYYMMDD_HHMMSS.log
/// File-only; never writes secrets or credential material.
class WorkerTaskLog final {
  public:
    WorkerTaskLog(const WorkerTaskLog&) = delete;
    WorkerTaskLog& operator=(const WorkerTaskLog&) = delete;
    WorkerTaskLog(WorkerTaskLog&&) = delete;
    WorkerTaskLog& operator=(WorkerTaskLog&&) = delete;
    ~WorkerTaskLog();

    /// Opens a new timestamped log under logs/<operation>/. Returns nullptr if open fails.
    [[nodiscard]] static std::unique_ptr<WorkerTaskLog> open(std::string_view operation) noexcept;

    /// Thread-local active task log for nested runtime milestones (may be null).
    [[nodiscard]] static WorkerTaskLog* active() noexcept;

    void info(std::string_view message) noexcept;
    void warn(std::string_view message) noexcept;
    void error(std::string_view message) noexcept;

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

} // namespace aegra::apps::worker
