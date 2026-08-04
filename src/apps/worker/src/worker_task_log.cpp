#include "worker_task_log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace aegra::apps::worker {
namespace {

thread_local WorkerTaskLog* g_active_task_log = nullptr;

[[nodiscard]] std::filesystem::path environment_path(const wchar_t* name) {
    const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }
    std::vector<wchar_t> value(required);
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) {
        return {};
    }
    return std::filesystem::path(value.data());
}

[[nodiscard]] std::filesystem::path resolve_data_dir() {
    // Prefer the directory Service published for this process tree.
    if (const auto from_service = environment_path(L"AEGRA_DATA_DIR"); !from_service.empty()) {
        return from_service;
    }
    // Interactive Service / developer runs use LOCALAPPDATA; SCM uses ProgramData.
    if (const auto local = environment_path(L"LOCALAPPDATA"); !local.empty()) {
        return local / L"Aegra";
    }
    if (const auto program_data = environment_path(L"ProgramData"); !program_data.empty()) {
        return program_data / L"Aegra";
    }
    return {};
}

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const auto& value = path.native();
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const auto input_size = static_cast<int>(value.size());
    const int required = ::WideCharToMultiByte(CP_UTF8, 0, value.data(), input_size, nullptr, 0,
                                               nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, value.data(), input_size, output.data(), required,
                              nullptr, nullptr) <= 0) {
        return {};
    }
    return output;
}

[[nodiscard]] std::string timestamp_filename() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    if (::localtime_s(&local, &time) != 0) {
        return "task.log";
    }
    char buffer[32]{};
    if (std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d_%02d%02d%02d.log", local.tm_year + 1900,
                      local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                      local.tm_sec) <= 0) {
        return "task.log";
    }
    return buffer;
}

[[nodiscard]] bool safe_operation_name(const std::string_view operation) noexcept {
    if (operation.empty() || operation.size() > 32) {
        return false;
    }
    for (const unsigned char character : operation) {
        const bool ok = (character >= 'a' && character <= 'z') ||
                        (character >= '0' && character <= '9') || character == '_' ||
                        character == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

struct WorkerTaskLog::Impl final {
    std::shared_ptr<spdlog::logger> logger;
    std::string path;
};

WorkerTaskLog::WorkerTaskLog(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

WorkerTaskLog::~WorkerTaskLog() {
    if (g_active_task_log == this) {
        g_active_task_log = nullptr;
    }
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return;
    }
    try {
        impl_->logger->info("========================================");
        impl_->logger->info("Log ended");
        impl_->logger->info("========================================");
        impl_->logger->flush();
        spdlog::drop(impl_->logger->name());
    } catch (...) {
    }
}

WorkerTaskLog* WorkerTaskLog::active() noexcept { return g_active_task_log; }

WorkerTaskLogScope::WorkerTaskLogScope(WorkerTaskLog* log) noexcept : previous_(g_active_task_log) {
    g_active_task_log = log;
}

WorkerTaskLogScope::~WorkerTaskLogScope() { g_active_task_log = previous_; }

std::unique_ptr<WorkerTaskLog> WorkerTaskLog::open(const std::string_view operation) noexcept {
    try {
        if (!safe_operation_name(operation)) {
            return nullptr;
        }
        const auto data_dir = resolve_data_dir();
        if (data_dir.empty()) {
            return nullptr;
        }
        std::error_code error;
        const auto log_dir = data_dir / L"logs" / std::filesystem::path(operation);
        std::filesystem::create_directories(log_dir, error);
        if (error) {
            return nullptr;
        }
        const auto file_path = log_dir / timestamp_filename();
        auto path_utf8 = path_to_utf8(file_path);
        if (path_utf8.empty()) {
            return nullptr;
        }
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path_utf8, true);
        static std::atomic_uint64_t counter{0};
        const auto name = "worker_task_" + std::to_string(++counter);
        auto logger = std::make_shared<spdlog::logger>(name, std::move(sink));
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        logger->info("========================================");
        logger->info("Log started; file: {}", path_utf8);
        logger->info("========================================");
        auto impl = std::make_unique<Impl>();
        impl->logger = std::move(logger);
        impl->path = std::move(path_utf8);
        return std::unique_ptr<WorkerTaskLog>(new WorkerTaskLog(std::move(impl)));
    } catch (...) {
        return nullptr;
    }
}

void WorkerTaskLog::info(const std::string_view message) noexcept {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return;
    }
    try {
        impl_->logger->info("{}", message);
    } catch (...) {
    }
}

void WorkerTaskLog::warn(const std::string_view message) noexcept {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return;
    }
    try {
        impl_->logger->warn("{}", message);
    } catch (...) {
    }
}

void WorkerTaskLog::error(const std::string_view message) noexcept {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return;
    }
    try {
        impl_->logger->error("{}", message);
    } catch (...) {
    }
}

std::string_view WorkerTaskLog::path() const noexcept {
    return impl_ != nullptr ? std::string_view{impl_->path} : std::string_view{};
}

} // namespace aegra::apps::worker
