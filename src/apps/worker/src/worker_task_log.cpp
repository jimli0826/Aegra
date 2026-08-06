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

// Fixed key column so every `  key : value` line places `:` at the same offset.
// Must cover the longest keys (e.g. exclude_page_and_hibernation_files).
constexpr std::size_t kFieldKeyWidth = 36;

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

[[nodiscard]] std::string timestamp_prefix() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    if (::localtime_s(&local, &time) != 0) {
        return "task";
    }
    char buffer[32]{};
    if (std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d_%02d%02d%02d", local.tm_year + 1900,
                      local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                      local.tm_sec) <= 0) {
        return "task";
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

[[nodiscard]] std::string sanitize_job_id_for_filename(const std::string_view job_id) {
    if (job_id.empty() || job_id.size() > 80) {
        return {};
    }
    std::string out;
    out.reserve(job_id.size());
    for (const unsigned char character : job_id) {
        const bool ok = (character >= 'a' && character <= 'z') ||
                        (character >= 'A' && character <= 'Z') ||
                        (character >= '0' && character <= '9') || character == '_' ||
                        character == '-';
        if (!ok) {
            return {};
        }
        out.push_back(static_cast<char>(character));
    }
    return out;
}

[[nodiscard]] std::string make_log_filename(const std::string_view job_id) {
    auto name = timestamp_prefix();
    if (const auto safe = sanitize_job_id_for_filename(job_id); !safe.empty()) {
        name.push_back('_');
        name.append(safe);
    }
    name.append(".log");
    return name;
}

/// Pads or truncates `key` to exactly kFieldKeyWidth so colons stay column-aligned.
[[nodiscard]] std::string pad_key(const std::string_view key) {
    if (key.size() == kFieldKeyWidth) {
        return std::string(key);
    }
    if (key.size() < kFieldKeyWidth) {
        std::string out(key);
        out.append(kFieldKeyWidth - key.size(), ' ');
        return out;
    }
    // Truncate with ellipsis when a key exceeds the column (keeps colon aligned).
    if (kFieldKeyWidth <= 3) {
        return std::string(kFieldKeyWidth, '.');
    }
    std::string out;
    out.reserve(kFieldKeyWidth);
    out.append(key.data(), kFieldKeyWidth - 3);
    out.append("...");
    return out;
}

void write_line(spdlog::logger& logger, const spdlog::level::level_enum level,
                const std::string_view message) {
    logger.log(level, "{}", message);
}

} // namespace

std::string format_human_bytes(const std::uint64_t bytes) {
    constexpr double kKib = 1024.0;
    constexpr double kMib = kKib * 1024.0;
    constexpr double kGib = kMib * 1024.0;
    char human[64]{};
    if (bytes >= static_cast<std::uint64_t>(kGib)) {
        std::snprintf(human, sizeof(human), "%.2f GiB", static_cast<double>(bytes) / kGib);
    } else if (bytes >= static_cast<std::uint64_t>(kMib)) {
        std::snprintf(human, sizeof(human), "%.2f MiB", static_cast<double>(bytes) / kMib);
    } else if (bytes >= static_cast<std::uint64_t>(kKib)) {
        std::snprintf(human, sizeof(human), "%.2f KiB", static_cast<double>(bytes) / kKib);
    } else {
        std::snprintf(human, sizeof(human), "%llu B", static_cast<unsigned long long>(bytes));
        return human;
    }
    char full[96]{};
    std::snprintf(full, sizeof(full), "%s (%llu bytes)", human,
                  static_cast<unsigned long long>(bytes));
    return full;
}

std::string format_duration_ms(const std::chrono::milliseconds elapsed) {
    char buffer[64]{};
    if (elapsed.count() < 1000) {
        std::snprintf(buffer, sizeof(buffer), "%lld ms",
                      static_cast<long long>(elapsed.count()));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.3f s",
                      static_cast<double>(elapsed.count()) / 1000.0);
    }
    return buffer;
}

std::string path_display(const std::filesystem::path& path) {
    auto utf8 = path_to_utf8(path);
    return utf8.empty() ? path.string() : utf8;
}

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

std::unique_ptr<WorkerTaskLog> WorkerTaskLog::open(const std::string_view operation,
                                                   const std::string_view job_id) noexcept {
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
        const auto file_path = log_dir / make_log_filename(job_id);
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
        // %-5l keeps [info]/[warn]/[error] the same width so field colons stay aligned.
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%-5l] %v");
        logger->info("========================================");
        logger->info("Aegra Worker Task Log");
        logger->info("  {} : {}", pad_key("operation"), operation);
        logger->info("  {} : {}", pad_key("file"), path_utf8);
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
        write_line(*impl_->logger, spdlog::level::info, message);
    } catch (...) {
    }
}

void WorkerTaskLog::warn(const std::string_view message) noexcept {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return;
    }
    try {
        write_line(*impl_->logger, spdlog::level::warn, message);
    } catch (...) {
    }
}

void WorkerTaskLog::error(const std::string_view message) noexcept {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return;
    }
    try {
        write_line(*impl_->logger, spdlog::level::err, message);
    } catch (...) {
    }
}

void WorkerTaskLog::section(const std::string_view title) noexcept {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return;
    }
    try {
        impl_->logger->info("");
        impl_->logger->info("[{}]", title);
    } catch (...) {
    }
}

void WorkerTaskLog::field(const std::string_view key, const std::string_view value) noexcept {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return;
    }
    try {
        // Fixed-width key column: "  <key padded to 36> : <value>"
        impl_->logger->info("  {} : {}", pad_key(key), value);
    } catch (...) {
    }
}

void WorkerTaskLog::field_u64(const std::string_view key, const std::uint64_t value) noexcept {
    field(key, std::to_string(value));
}

void WorkerTaskLog::field_bool(const std::string_view key, const bool value) noexcept {
    field(key, value ? "true" : "false");
}

void WorkerTaskLog::field_bytes(const std::string_view key, const std::uint64_t bytes) noexcept {
    field(key, format_human_bytes(bytes));
}

void WorkerTaskLog::stage_begin(const std::string_view stage) noexcept {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return;
    }
    try {
        impl_->logger->info("");
        impl_->logger->info("[Stage: {}] begin", stage);
    } catch (...) {
    }
}

void WorkerTaskLog::stage_ok(const std::string_view stage,
                             const std::chrono::milliseconds elapsed) noexcept {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return;
    }
    try {
        impl_->logger->info("[Stage: {}] OK ({})", stage, format_duration_ms(elapsed));
    } catch (...) {
    }
}

void WorkerTaskLog::stage_fail(const std::string_view stage,
                               const std::chrono::milliseconds elapsed, const base::Error& error,
                               const std::string_view step, const std::string_view hint) noexcept {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return;
    }
    try {
        impl_->logger->error("[Stage: {}] FAILED ({})", stage, format_duration_ms(elapsed));
        if (!step.empty()) {
            field("step", step);
        }
        field("error_code", base::error_code_name(error.code));
        if (!error.message.empty()) {
            field("error_message", error.message);
        }
        if (!hint.empty()) {
            field("hint", hint);
        }
    } catch (...) {
    }
}

std::string_view WorkerTaskLog::path() const noexcept {
    return impl_ != nullptr ? std::string_view{impl_->path} : std::string_view{};
}

ScopedStage::ScopedStage(WorkerTaskLog* log, const std::string_view stage) noexcept
    : log_(log), stage_(stage), started_(std::chrono::steady_clock::now()) {
    if (log_ != nullptr) {
        log_->stage_begin(stage_);
    }
}

ScopedStage::~ScopedStage() {
    if (finished_ || log_ == nullptr) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_);
    log_->stage_ok(stage_, elapsed);
}

void ScopedStage::note(const std::string_view key, const std::string_view value) noexcept {
    if (log_ != nullptr) {
        log_->field(key, value);
    }
}

void ScopedStage::note_u64(const std::string_view key, const std::uint64_t value) noexcept {
    if (log_ != nullptr) {
        log_->field_u64(key, value);
    }
}

void ScopedStage::note_bool(const std::string_view key, const bool value) noexcept {
    if (log_ != nullptr) {
        log_->field_bool(key, value);
    }
}

void ScopedStage::note_bytes(const std::string_view key, const std::uint64_t bytes) noexcept {
    if (log_ != nullptr) {
        log_->field_bytes(key, bytes);
    }
}

void ScopedStage::fail(const base::Error& error, const std::string_view step,
                       const std::string_view hint) noexcept {
    if (finished_) {
        return;
    }
    finished_ = true;
    failed_ = true;
    if (log_ == nullptr) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_);
    log_->stage_fail(stage_, elapsed, error, step, hint);
}

} // namespace aegra::apps::worker
