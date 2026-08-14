#include "aegra/adapters/sqlite/sqlite_control_plane.h"
#include "aegra/adapters/storage_local/local_object_storage.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/adapters/windows_filesystem/windows_filesystem.h"
#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/adapters/windows_process/windows_process_launcher.h"
#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/application/connected_repository_query.h"
#include "aegra/application/file_browse_service.h"
#include "aegra/application/personal_repository_query.h"
#include "aegra/application/recovery_point_operations.h"
#include "aegra/application/repository_connection_service.h"
#include "aegra/application/source_inventory_query.h"
#include "aegra/apps/service/backup_catalog_registrar.h"
#include "aegra/apps/service/mount_supervisor.h"
#include "aegra/apps/service/schedule_service.h"
#include "aegra/apps/service/service_host.h"
#include "aegra/apps/service/service_protocol.h"
#include "aegra/apps/service/service_security_host.h"
#include "aegra/apps/service/windows_service_scm_host.h"
#include "aegra/apps/service/worker_job_service.h"
#include "aegra/apps/service/worker_supervisor.h"
#include "aegra/base/error.h"
#include "aegra/ports/repository_storage.h"
#include "network_aware_storage_factory.h"
#include "network_share_access.h"
#include "service_log_formatter.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace service = aegra::apps::service;
namespace sqlite = aegra::adapters::sqlite;
namespace storage_local = aegra::adapters::storage_local;
namespace windows_disk = aegra::adapters::windows_disk;
namespace windows_filesystem = aegra::adapters::windows_filesystem;
namespace windows_ipc = aegra::adapters::windows_ipc;
namespace windows_process = aegra::adapters::windows_process;
namespace windows_system = aegra::adapters::windows_system;

constexpr std::wstring_view kServiceName = L"AegraService";

enum class ServiceExitCode : int {
    kSucceeded = 0,
    kInvalidArguments = 20,
    kHostFailure = 21,
};

[[nodiscard]] spdlog::level::level_enum log_level(const service::ServiceLogLevel level) noexcept {
    switch (level) {
    case service::ServiceLogLevel::kTrace:
        return spdlog::level::trace;
    case service::ServiceLogLevel::kInfo:
        return spdlog::level::info;
    case service::ServiceLogLevel::kWarning:
        return spdlog::level::warn;
    case service::ServiceLogLevel::kError:
        return spdlog::level::err;
    }
    return spdlog::level::info;
}

// One rotating file per level so operators can open error.log / warning.log without noise.
// critical is folded into error.log (ServiceLogLevel has no critical).
class LevelOnlyFileSink final : public spdlog::sinks::base_sink<std::mutex> {
  public:
    LevelOnlyFileSink(spdlog::filename_t path, const spdlog::level::level_enum only_level)
        : only_level_(only_level), file_(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                                       std::move(path), 10U * 1024U * 1024U, 5U)) {
        set_level(only_level);
        file_->set_level(spdlog::level::trace);
    }

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (!accepts(msg.level)) {
            return;
        }
        file_->log(msg);
    }

    void flush_() override { file_->flush(); }

    void set_pattern_(const std::string& pattern) override {
        file_->set_pattern(pattern);
        spdlog::sinks::base_sink<std::mutex>::set_pattern_(pattern);
    }

    void set_formatter_(std::unique_ptr<spdlog::formatter> sink_formatter) override {
        file_->set_formatter(sink_formatter->clone());
        spdlog::sinks::base_sink<std::mutex>::set_formatter_(std::move(sink_formatter));
    }

  private:
    [[nodiscard]] bool accepts(const spdlog::level::level_enum level) const noexcept {
        if (level == only_level_) {
            return true;
        }
        return only_level_ == spdlog::level::err && level == spdlog::level::critical;
    }

    spdlog::level::level_enum only_level_;
    std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> file_;
};

[[nodiscard]] std::string_view
job_state_name(const aegra::contracts::ServiceJobState state) noexcept {
    switch (state) {
    case aegra::contracts::ServiceJobState::kQueued:
        return "Queued";
    case aegra::contracts::ServiceJobState::kRunning:
        return "Running";
    case aegra::contracts::ServiceJobState::kCancelling:
        return "Cancelling";
    case aegra::contracts::ServiceJobState::kSucceeded:
        return "Succeeded";
    case aegra::contracts::ServiceJobState::kFailed:
        return "Failed";
    case aegra::contracts::ServiceJobState::kCancelled:
        return "Cancelled";
    case aegra::contracts::ServiceJobState::kInterrupted:
        return "Interrupted";
    }
    return "Unknown";
}

class SpdlogServiceLog final : public service::IServiceLog {
  public:
    explicit SpdlogServiceLog(std::shared_ptr<spdlog::logger> logger) : logger_(std::move(logger)) {
        logger_->info("Service log started. [event=service.log_started]");
    }

    ~SpdlogServiceLog() override {
        logger_->info("Service log stopped. [event=service.log_stopped]");
        logger_->flush();
    }

    void write(const service::ServiceLogLevel level, const std::string_view message_code,
               const std::string_view detail) noexcept override {
        try {
            logger_->log(log_level(level), "{}: {} [event={}]",
                         service::detail::readable_code(message_code), detail, message_code);
        } catch (...) {
        }
    }

  private:
    std::shared_ptr<spdlog::logger> logger_;
};

struct ServiceArguments final {
    bool once{false};
    bool service_mode{false};
    std::string pipe_name{"control"};
    std::optional<std::filesystem::path> repository_root;
    std::optional<std::filesystem::path> data_dir;
    std::optional<std::filesystem::path> worker_path;
    std::optional<std::filesystem::path> mount_host_path;
};

struct RuntimeComponents final {
    std::unique_ptr<service::IServiceLog> logger;
    std::unique_ptr<windows_system::WindowsSystemClock> clock;
    std::unique_ptr<windows_system::WindowsCryptographicRandom> random;
    std::unique_ptr<windows_disk::WindowsSourceInventory> source_inventory;
    std::unique_ptr<storage_local::LocalRepositoryStorageFactory> local_storage_factory;
    std::unique_ptr<aegra::ports::IRepositoryStorageFactory> storage_factory;
    std::unique_ptr<windows_process::WindowsProcessLauncher> process_launcher;
    std::unique_ptr<storage_local::LocalObjectStorage> storage;
    std::unique_ptr<aegra::application::PersonalRepositoryQuery> repository_query;
    std::unique_ptr<sqlite::SqliteControlPlaneDatabase> control_plane;
    std::unique_ptr<aegra::application::SourceInventoryQuery> source_query;
    std::unique_ptr<aegra::application::RepositoryConnectionService> connection_service;
    std::unique_ptr<aegra::application::ConnectedRepositoryQuery> connected_query;
    std::unique_ptr<aegra::application::RecoveryPointOperations> recovery_point_operations;
    std::shared_ptr<service::BackupCatalogRegistrar> backup_catalog_registrar;
    std::unique_ptr<windows_filesystem::WindowsFileSourceBrowser> file_browser;
    std::unique_ptr<aegra::application::FileBrowseService> file_browse;
    std::unique_ptr<service::RepositoryLocationBrowseRegistry> repository_location_browse;
    std::unique_ptr<service::WorkerSupervisor> supervisor;
    std::unique_ptr<service::WorkerJobService> worker_jobs;
    std::unique_ptr<service::ScheduleService> schedules;
    std::unique_ptr<service::MountSupervisor> mount_supervisor;
    service::ServiceRuntimeInfo runtime;
};

class WindowsServiceStatusReporter final : public service::IWindowsServiceStatusReporter {
  public:
    explicit WindowsServiceStatusReporter(const SERVICE_STATUS_HANDLE handle) : handle_(handle) {}

    [[nodiscard]] aegra::base::Result<void>
    report(const service::WindowsServiceStatus& status) override {
        SERVICE_STATUS native_status{};
        native_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        native_status.dwCurrentState = native_state(status.state);
        native_status.dwControlsAccepted = status.accepts_stop ? SERVICE_ACCEPT_STOP : 0;
        native_status.dwWin32ExitCode = status.win32_exit_code;
        native_status.dwServiceSpecificExitCode = status.service_exit_code;
        native_status.dwWaitHint = status.wait_hint_ms;
        if (!::SetServiceStatus(handle_, &native_status)) {
            return aegra::base::Result<void>::failure(
                {aegra::base::ErrorCode::kInternal, "SetServiceStatus failed"});
        }
        return aegra::base::Result<void>::success();
    }

  private:
    [[nodiscard]] static DWORD native_state(const service::WindowsServiceState state) noexcept {
        switch (state) {
        case service::WindowsServiceState::kStopped:
            return SERVICE_STOPPED;
        case service::WindowsServiceState::kStartPending:
            return SERVICE_START_PENDING;
        case service::WindowsServiceState::kRunning:
            return SERVICE_RUNNING;
        case service::WindowsServiceState::kStopPending:
            return SERVICE_STOP_PENDING;
        }
        return SERVICE_STOPPED;
    }

    SERVICE_STATUS_HANDLE handle_;
};

[[nodiscard]] std::optional<std::string> narrow_ascii(const std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        if (character > 0x7F)
            return std::nullopt;
        result.push_back(static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] bool parse_arguments(const std::span<const wchar_t* const> arguments,
                                   ServiceArguments& result) {
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::wstring_view argument = arguments[index];
        if (argument == L"--once" && !result.once) {
            result.once = true;
        } else if (argument == L"--service" && !result.service_mode) {
            result.service_mode = true;
        } else if (argument == L"--pipe" && index + 1 < arguments.size()) {
            auto pipe_name = narrow_ascii(arguments[++index]);
            if (!pipe_name)
                return false;
            result.pipe_name = std::move(*pipe_name);
        } else if (argument == L"--repository-root" && index + 1 < arguments.size() &&
                   !result.repository_root) {
            result.repository_root = std::filesystem::path(arguments[++index]);
        } else if (argument == L"--data-dir" && index + 1 < arguments.size() && !result.data_dir) {
            result.data_dir = std::filesystem::path(arguments[++index]);
        } else if (argument == L"--worker-path" && index + 1 < arguments.size() &&
                   !result.worker_path) {
            result.worker_path = std::filesystem::path(arguments[++index]);
        } else if (argument == L"--mount-host-path" && index + 1 < arguments.size() &&
                   !result.mount_host_path) {
            result.mount_host_path = std::filesystem::path(arguments[++index]);
        } else {
            return false;
        }
    }
    const auto absolute = [](const auto& value) { return !value || value->is_absolute(); };
    return absolute(result.repository_root) && absolute(result.data_dir) &&
           absolute(result.worker_path) && absolute(result.mount_host_path);
}

[[nodiscard]] aegra::base::Result<std::filesystem::path>
environment_directory(const wchar_t* variable_name) {
    const DWORD required = ::GetEnvironmentVariableW(variable_name, nullptr, 0);
    if (required == 0) {
        return aegra::base::Result<std::filesystem::path>::failure(
            {aegra::base::ErrorCode::kNotFound, "required data directory environment is missing"});
    }
    std::vector<wchar_t> value(required);
    const DWORD written = ::GetEnvironmentVariableW(variable_name, value.data(), required);
    if (written == 0 || written >= required) {
        return aegra::base::Result<std::filesystem::path>::failure(
            {aegra::base::ErrorCode::kInternal, "failed to read data directory environment"});
    }
    return aegra::base::Result<std::filesystem::path>::success(std::filesystem::path(value.data()) /
                                                               L"Aegra");
}

[[nodiscard]] aegra::base::Result<std::filesystem::path>
resolve_data_dir(const ServiceArguments& arguments) {
    if (arguments.data_dir) {
        return aegra::base::Result<std::filesystem::path>::success(*arguments.data_dir);
    }
    return environment_directory(arguments.service_mode ? L"ProgramData" : L"LOCALAPPDATA");
}

[[nodiscard]] aegra::base::Result<std::filesystem::path>
resolve_sibling_executable(const std::optional<std::filesystem::path>& explicit_path,
                           const wchar_t* sibling_name) {
    if (explicit_path) {
        return aegra::base::Result<std::filesystem::path>::success(*explicit_path);
    }
    std::vector<wchar_t> module_path(32'768);
    const DWORD length =
        ::GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0 || static_cast<std::size_t>(length) >= module_path.size()) {
        return aegra::base::Result<std::filesystem::path>::failure(
            {aegra::base::ErrorCode::kInternal, "failed to resolve Service executable path"});
    }
    const std::filesystem::path executable(std::wstring_view(module_path.data(), length));
    return aegra::base::Result<std::filesystem::path>::success(executable.parent_path() /
                                                               sibling_name);
}

[[nodiscard]] aegra::base::Result<std::filesystem::path>
resolve_worker_path(const ServiceArguments& arguments) {
    return resolve_sibling_executable(arguments.worker_path, L"aegra_personal_worker.exe");
}

[[nodiscard]] aegra::base::Result<std::filesystem::path>
resolve_mount_host_path(const ServiceArguments& arguments) {
    return resolve_sibling_executable(arguments.mount_host_path, L"aegra_mount_host.exe");
}

[[nodiscard]] aegra::base::Result<std::string> path_to_utf8(const std::filesystem::path& path) {
    const auto& value = path.native();
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return aegra::base::Result<std::string>::failure(
            {aegra::base::ErrorCode::kInvalidArgument, "Worker executable path is invalid"});
    }
    const auto input_size = static_cast<int>(value.size());
    const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                               input_size, nullptr, 0, nullptr, nullptr);
    if (required == 0) {
        return aegra::base::Result<std::string>::failure(
            {aegra::base::ErrorCode::kInvalidArgument, "Worker executable path is invalid"});
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
                              output.data(), required, nullptr, nullptr) == 0) {
        return aegra::base::Result<std::string>::failure(
            {aegra::base::ErrorCode::kInvalidArgument, "Worker executable path is invalid"});
    }
    return aegra::base::Result<std::string>::success(std::move(output));
}

[[nodiscard]] aegra::base::Result<std::shared_ptr<spdlog::sinks::sink>>
make_level_log_sink(const std::filesystem::path& log_dir, const wchar_t* filename,
                    const spdlog::level::level_enum only_level) {
    auto path = path_to_utf8(log_dir / filename);
    if (!path) {
        return aegra::base::Result<std::shared_ptr<spdlog::sinks::sink>>::failure(path.error());
    }
    try {
        return aegra::base::Result<std::shared_ptr<spdlog::sinks::sink>>::success(
            std::make_shared<LevelOnlyFileSink>(path.value(), only_level));
    } catch (const spdlog::spdlog_ex&) {
        return aegra::base::Result<std::shared_ptr<spdlog::sinks::sink>>::failure(
            {aegra::base::ErrorCode::kIoFailure, "failed to open Service level log file"});
    }
}

[[nodiscard]] aegra::base::Result<std::unique_ptr<service::IServiceLog>>
create_service_log(const std::filesystem::path& data_dir, const bool service_mode) {
    std::error_code error;
    const auto log_dir = data_dir / L"logs";
    std::filesystem::create_directories(log_dir, error);
    if (error) {
        return aegra::base::Result<std::unique_ptr<service::IServiceLog>>::failure(
            {aegra::base::ErrorCode::kIoFailure, "failed to create Service log directory"});
    }
    // Split by level: logs/trace.log, info.log, warning.log, error.log (no combined service.log).
    const std::array<std::pair<const wchar_t*, spdlog::level::level_enum>, 4> level_files{{
        {L"trace.log", spdlog::level::trace},
        {L"info.log", spdlog::level::info},
        {L"warning.log", spdlog::level::warn},
        {L"error.log", spdlog::level::err},
    }};
    std::vector<spdlog::sink_ptr> sinks;
    sinks.reserve(level_files.size());
    for (const auto& [filename, level] : level_files) {
        auto sink = make_level_log_sink(log_dir, filename, level);
        if (!sink) {
            return aegra::base::Result<std::unique_ptr<service::IServiceLog>>::failure(
                sink.error());
        }
        sinks.push_back(std::move(sink).value());
    }
    try {
        auto logger = std::make_shared<spdlog::logger>("aegra_service", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::info);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        (void)service_mode; // retained for future SCM-specific log policy
        return aegra::base::Result<std::unique_ptr<service::IServiceLog>>::success(
            std::make_unique<SpdlogServiceLog>(std::move(logger)));
    } catch (const spdlog::spdlog_ex&) {
        return aegra::base::Result<std::unique_ptr<service::IServiceLog>>::failure(
            {aegra::base::ErrorCode::kIoFailure, "failed to initialize Service logger"});
    }
}

[[nodiscard]] aegra::base::Result<void> open_control_plane(const std::filesystem::path& data_dir,
                                                           RuntimeComponents& components) {
    std::error_code error;
    std::filesystem::create_directories(data_dir, error);
    if (error) {
        return aegra::base::Result<void>::failure(
            {aegra::base::ErrorCode::kIoFailure, "failed to create Service data directory"});
    }
    auto opened = sqlite::SqliteControlPlaneDatabase::open(
        {data_dir / L"control-plane.db", sqlite::SqliteOpenMode::kCreateIfMissing});
    if (!opened)
        return aegra::base::Result<void>::failure(opened.error());
    components.control_plane = std::move(opened).value();
    auto unit = components.control_plane->begin_unit_of_work({});
    if (!unit)
        return aegra::base::Result<void>::failure(unit.error());
    const auto now_signed = components.clock->now_utc_ms();
    const auto now_utc_ms = now_signed > 0 ? static_cast<std::uint64_t>(now_signed) : 0ULL;
    auto interrupted = unit.value()->jobs().mark_active_as_interrupted(now_utc_ms, {});
    if (!interrupted) {
        unit.value()->rollback();
        return aegra::base::Result<void>::failure(interrupted.error());
    }
    auto settings = unit.value()->service_settings().get({});
    if (!settings) {
        unit.value()->rollback();
        return aegra::base::Result<void>::failure(settings.error());
    }
    const auto window = static_cast<std::uint64_t>(settings.value().job_retention_months) *
                        aegra::contracts::kMillisecondsPerRetentionMonth;
    const auto cutoff = now_utc_ms > window ? now_utc_ms - window : 0ULL;
    auto purged = unit.value()->jobs().purge_terminal_completed_before(cutoff, {});
    if (!purged) {
        unit.value()->rollback();
        return aegra::base::Result<void>::failure(purged.error());
    }
    return unit.value()->commit({});
}

[[nodiscard]] aegra::base::Result<void> open_legacy_repository(const ServiceArguments& arguments,
                                                               RuntimeComponents& components) {
    if (!arguments.repository_root) {
        return aegra::base::Result<void>::success();
    }
    auto opened = storage_local::LocalObjectStorage::open(
        {*arguments.repository_root, storage_local::LocalRootMode::kOpenExisting});
    if (!opened)
        return aegra::base::Result<void>::failure(opened.error());
    components.storage = std::move(opened).value();
    components.repository_query = std::make_unique<aegra::application::PersonalRepositoryQuery>(
        *components.storage, *components.storage);
    return aegra::base::Result<void>::success();
}

[[nodiscard]] std::vector<std::string> runtime_capabilities(const bool file_browse_enabled) {
    // Chain/delete stay off until durable delete resume meets S5 Definition of Done.
    // F8 enables file.restore (PrepareFileRestore + StartFileRestore) when browse is available.
    std::vector<std::string> capabilities{
        "backup.start",
        "job.cancel",
        "job.list",
        "mount.list",
        "mount.start",
        "mount.unmount",
        "recovery_point.verify",
        "repository.connection",
        "repository.list",
        "restore.preflight",
        "restore.start",
        "schedule",
        "service.info",
        "service.settings",
        "source.inventory",
        "file.recover_browse",
    };
    if (file_browse_enabled) {
        capabilities.push_back("file.browse");
        capabilities.push_back("file.restore");
        capabilities.push_back("schedule.file_set");
    }
    std::ranges::sort(capabilities);
    return capabilities;
}

[[nodiscard]] std::vector<std::uint16_t> utf8_to_utf16_units(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                               static_cast<int>(utf8.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                              static_cast<int>(utf8.size()), wide.data(), required) != required) {
        return {};
    }
    while (!wide.empty() && (wide.back() == L'\\' || wide.back() == L'/')) {
        wide.pop_back();
    }
    return std::vector<std::uint16_t>(wide.begin(), wide.end());
}

/// True when inventory mount_letter is a drive root like "C:" (not a folder mount).
[[nodiscard]] bool is_drive_letter_mount(const std::string_view mount) noexcept {
    if (mount.size() != 2 || mount[1] != ':') {
        return false;
    }
    const auto letter = static_cast<unsigned char>(mount[0]);
    return (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
}

/// UI label for file browse roots: "Label (D:)" or "D:" when the label is empty.
[[nodiscard]] std::string
browse_root_display_name(const aegra::ports::SourceInventoryRecord& source) {
    std::string letter(source.mount_letter);
    if (letter.size() == 2) {
        letter[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(letter[0])));
    }
    const auto& label = source.display_name;
    if (!label.empty() && label != letter && label != source.mount_letter) {
        return label + " (" + letter + ")";
    }
    return letter;
}

[[nodiscard]] aegra::base::Result<std::vector<windows_filesystem::SnapshotVolumeBinding>>
build_browse_roots(aegra::ports::ISourceInventory& inventory) {
    auto sources = inventory.list_sources({});
    if (!sources) {
        return aegra::base::Result<std::vector<windows_filesystem::SnapshotVolumeBinding>>::failure(
            sources.error());
    }
    std::vector<windows_filesystem::SnapshotVolumeBinding> roots;
    roots.reserve(sources.value().size());
    for (const auto& source : sources.value()) {
        // File browse only exposes volumes with a drive letter. Hidden system partitions
        // (EFI, MSR, Recovery, unmounted) have empty mount_letter and stay out of the tree.
        if (source.source_id.rfind("vol.", 0) != 0 || source.stable_key.empty() ||
            source.availability != aegra::contracts::SourceAvailability::kAvailable ||
            source.capacity_bytes == 0 || !is_drive_letter_mount(source.mount_letter)) {
            continue;
        }
        auto path_utf16 = utf8_to_utf16_units(source.stable_key);
        if (path_utf16.empty()) {
            continue;
        }
        windows_filesystem::SnapshotVolumeBinding root;
        root.volume_identity = source.stable_key;
        root.snapshot_root_utf16 = std::move(path_utf16);
        root.display_name = browse_root_display_name(source);
        roots.push_back(std::move(root));
    }
    return aegra::base::Result<std::vector<windows_filesystem::SnapshotVolumeBinding>>::success(
        std::move(roots));
}

[[nodiscard]] aegra::base::Result<RuntimeComponents>
create_runtime(const ServiceArguments& arguments) {
    RuntimeComponents components;
    components.clock = std::make_unique<windows_system::WindowsSystemClock>();
    components.random = std::make_unique<windows_system::WindowsCryptographicRandom>();
    components.source_inventory = std::make_unique<windows_disk::WindowsSourceInventory>();
    components.local_storage_factory =
        std::make_unique<storage_local::LocalRepositoryStorageFactory>();
    components.process_launcher = std::make_unique<windows_process::WindowsProcessLauncher>();

    auto data_dir = resolve_data_dir(arguments);
    auto worker_path = resolve_worker_path(arguments);
    auto mount_host_path = resolve_mount_host_path(arguments);
    if (!data_dir || !worker_path || !mount_host_path) {
        return aegra::base::Result<RuntimeComponents>::failure(
            !data_dir ? data_dir.error()
                      : (!worker_path ? worker_path.error() : mount_host_path.error()));
    }
    // Worker inherits this environment and writes per-task logs under <data_dir>/logs/<op>/.
    if (!::SetEnvironmentVariableW(L"AEGRA_DATA_DIR", data_dir.value().c_str())) {
        return aegra::base::Result<RuntimeComponents>::failure(
            {aegra::base::ErrorCode::kInternal, "failed to publish AEGRA_DATA_DIR for Worker"});
    }
    auto logger = create_service_log(data_dir.value(), arguments.service_mode);
    if (!logger) {
        return aegra::base::Result<RuntimeComponents>::failure(logger.error());
    }
    components.logger = std::move(logger).value();
    components.logger->write(service::ServiceLogLevel::kInfo, "service.runtime_starting",
                             "status=starting");
    auto worker_path_utf8 = path_to_utf8(worker_path.value());
    if (!worker_path_utf8) {
        return aegra::base::Result<RuntimeComponents>::failure(worker_path_utf8.error());
    }
    auto mount_host_path_utf8 = path_to_utf8(mount_host_path.value());
    if (!mount_host_path_utf8) {
        return aegra::base::Result<RuntimeComponents>::failure(mount_host_path_utf8.error());
    }
    auto control_plane = open_control_plane(data_dir.value(), components);
    auto repository = open_legacy_repository(arguments, components);
    if (!control_plane || !repository) {
        return aegra::base::Result<RuntimeComponents>::failure(
            !control_plane ? control_plane.error() : repository.error());
    }

    components.storage_factory = std::make_unique<service::NetworkAwareRepositoryStorageFactory>(
        *components.local_storage_factory, *components.control_plane);
    components.source_query =
        std::make_unique<aegra::application::SourceInventoryQuery>(*components.source_inventory);
    components.connection_service =
        std::make_unique<aegra::application::RepositoryConnectionService>(
            *components.control_plane, *components.storage_factory, *components.clock,
            *components.random);
    components.connected_query = std::make_unique<aegra::application::ConnectedRepositoryQuery>(
        *components.control_plane, *components.storage_factory);
    components.recovery_point_operations =
        std::make_unique<aegra::application::RecoveryPointOperations>(
            *components.control_plane, *components.storage_factory, *components.clock,
            *components.random);
    components.backup_catalog_registrar = std::make_shared<service::BackupCatalogRegistrar>(
        *components.control_plane, *components.storage_factory);
    service::WorkerSupervisorConfig supervisor_config;
    supervisor_config.worker_executable_path = std::move(worker_path_utf8).value();
    // Job lifecycle runs async after backup.start is accepted — log terminal outcomes to file.
    auto* log = components.logger.get();
    auto* jobs_db = components.control_plane.get();
    auto catalog_registrar = components.backup_catalog_registrar;
    components.supervisor = std::make_unique<service::WorkerSupervisor>(
        std::move(supervisor_config), *components.process_launcher, *components.control_plane,
        *components.clock, *components.random, service::SupervisorProgressCallback{},
        [log, jobs_db, catalog_registrar](const service::WorkerJobRequest& request,
                                          const aegra::contracts::ServiceJobState final_state,
                                          const aegra::contracts::WorkerResponse* response) {
            const auto& job_id = request.worker_request.job_id;
            if (response != nullptr && catalog_registrar != nullptr) {
                auto registered = catalog_registrar->publish(request, *response, {});
                if (!registered && log != nullptr) {
                    std::string failure = "Catalog publication failed for job ";
                    failure += job_id;
                    failure += "; error=";
                    failure += service::detail::readable_code(
                        aegra::base::error_code_name(registered.error().code));
                    log->write(service::ServiceLogLevel::kError,
                               "repository.catalog_publish_failed", failure);
                }
            }
            if (log == nullptr) {
                return;
            }
            std::string detail = "Job ";
            detail += job_id;
            detail += " finished; state=";
            detail += job_state_name(final_state);
            if (jobs_db != nullptr) {
                auto job = jobs_db->get_job(job_id, {});
                if (job && job.value()) {
                    detail += "; status=";
                    detail += service::detail::readable_code(job.value()->message_code);
                    detail += " [";
                    detail += job.value()->message_code;
                    detail += ']';
                    if (job.value()->result_message_code) {
                        detail += "; result=";
                        detail += service::detail::readable_code(*job.value()->result_message_code);
                        detail += " [";
                        detail += *job.value()->result_message_code;
                        detail += ']';
                    }
                    if (job.value()->result_error_code) {
                        detail += "; error=";
                        detail += service::detail::readable_code(aegra::base::error_code_name(
                            static_cast<aegra::base::ErrorCode>(*job.value()->result_error_code)));
                    }
                }
            }
            const auto level = final_state == aegra::contracts::ServiceJobState::kSucceeded
                                   ? service::ServiceLogLevel::kInfo
                                   : service::ServiceLogLevel::kWarning;
            log->write(level, "job.terminal", detail);
        });
    bool file_browse_enabled = false;
    auto browse_roots = build_browse_roots(*components.source_inventory);
    if (browse_roots && !browse_roots.value().empty()) {
        auto browser =
            windows_filesystem::WindowsFileSourceBrowser::create(std::move(browse_roots).value());
        if (browser) {
            components.file_browser = std::move(browser).value();
            components.file_browse = std::make_unique<aegra::application::FileBrowseService>(
                *components.file_browser, *components.clock, *components.random);
            file_browse_enabled = true;
        } else if (components.logger != nullptr) {
            components.logger->write(service::ServiceLogLevel::kWarning, "file_browse.unavailable",
                                     "status=browser_create_failed");
        }
    } else if (components.logger != nullptr) {
        components.logger->write(service::ServiceLogLevel::kWarning, "file_browse.unavailable",
                                 "status=no_volume_roots");
    }
    components.worker_jobs = std::make_unique<service::WorkerJobService>(
        *components.source_query, *components.control_plane, *components.storage_factory,
        *components.supervisor, *components.clock, *components.random,
        components.file_browse.get());
    components.repository_location_browse =
        std::make_unique<service::RepositoryLocationBrowseRegistry>();
    components.schedules = std::make_unique<service::ScheduleService>(
        *components.control_plane, *components.clock, *components.random,
        components.file_browse.get());
    service::MountSupervisorConfig mount_config;
    mount_config.mount_host_executable_path = std::move(mount_host_path_utf8).value();
    mount_config.overlay_root = data_dir.value() / L"mount_overlays";
    components.mount_supervisor = std::make_unique<service::MountSupervisor>(
        std::move(mount_config), *components.process_launcher, *components.control_plane,
        *components.storage_factory, *components.clock, *components.random);
    components.runtime = {
        .service_version = AEGRA_APPLICATION_VERSION,
        .capabilities = runtime_capabilities(file_browse_enabled),
        .logger = components.logger.get(),
        .repository_query = components.repository_query.get(),
        .connected_repository_query = components.connected_query.get(),
        .repository_connections = components.connection_service.get(),
        .source_inventory = components.source_query.get(),
        .recovery_point_operations = components.recovery_point_operations.get(),
        .file_browse = components.file_browse.get(),
        .repository_location_browse = components.repository_location_browse.get(),
        .worker_jobs = components.worker_jobs.get(),
        .schedules = components.schedules.get(),
        .worker_supervisor = components.supervisor.get(),
        .mount_supervisor = components.mount_supervisor.get(),
        .control_plane = components.control_plane.get(),
        .storage_factory = components.storage_factory.get(),
    };
    components.logger->write(service::ServiceLogLevel::kInfo, "service.runtime_ready",
                             "status=ready");
    return aegra::base::Result<RuntimeComponents>::success(std::move(components));
}

[[nodiscard]] aegra::base::Result<std::unique_ptr<windows_ipc::WindowsNamedPipeListener>>
create_listener(const ServiceArguments& arguments) {
    return windows_ipc::WindowsNamedPipeListener::create(
        {arguments.pipe_name, static_cast<std::uint32_t>(service::kMaximumServiceFrameBytes),
         windows_ipc::WindowsNamedPipeAclProfile::kLocalEveryoneControl});
}

[[nodiscard]] service::ServiceSessionContext session_from_serial(const std::uint64_t serial) {
    service::ServiceSessionContext session;
    session.browser_session.session_id = "pipe-session-" + std::to_string(serial);
    return session;
}

[[nodiscard]] ServiceExitCode run_once(windows_ipc::WindowsNamedPipeListener& listener,
                                       const service::ServiceRuntimeInfo& runtime) {
    auto accepted = listener.accept({});
    if (!accepted)
        return ServiceExitCode::kHostFailure;
    if (runtime.logger != nullptr) {
        runtime.logger->write(service::ServiceLogLevel::kInfo, "service.session_started",
                              "mode=once");
    }
    const auto session = session_from_serial(1);
    auto result = service::run_service_session(*accepted.value(), runtime, session, {}, 1);
    if (runtime.logger != nullptr) {
        runtime.logger->write(
            result ? service::ServiceLogLevel::kInfo : service::ServiceLogLevel::kError,
            result ? "service.session_completed" : "service.session_failed", "mode=once");
    }
    return result ? ServiceExitCode::kSucceeded : ServiceExitCode::kHostFailure;
}

[[nodiscard]] ServiceExitCode run_forever(windows_ipc::WindowsNamedPipeListener& listener,
                                          const service::ServiceRuntimeInfo& runtime) {
    std::uint64_t serial = 1;
    for (;;) {
        auto accepted = listener.accept({});
        if (!accepted) {
            if (runtime.logger != nullptr) {
                runtime.logger->write(service::ServiceLogLevel::kError, "service.accept_failed",
                                      "mode=forever");
            }
            return ServiceExitCode::kHostFailure;
        }
        if (runtime.logger != nullptr) {
            runtime.logger->write(service::ServiceLogLevel::kInfo, "service.session_started",
                                  "mode=forever");
        }
        const auto session = session_from_serial(serial++);
        auto result = service::run_service_session(*accepted.value(), runtime, session, {});
        if (runtime.logger != nullptr) {
            runtime.logger->write(
                result ? service::ServiceLogLevel::kInfo : service::ServiceLogLevel::kWarning,
                result ? "service.session_completed" : "service.session_failed", "mode=forever");
        }
    }
}

[[nodiscard]] aegra::base::Result<ServiceArguments> parse_process_arguments() {
    int argument_count = 0;
    LPWSTR* raw_arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argument_count);
    if (raw_arguments == nullptr || argument_count <= 0) {
        return aegra::base::Result<ServiceArguments>::failure(
            {aegra::base::ErrorCode::kInvalidArgument, "failed to parse Service command line"});
    }
    std::unique_ptr<wchar_t*, decltype(&::LocalFree)> arguments(raw_arguments, &::LocalFree);
    std::vector<const wchar_t*> argument_view;
    argument_view.reserve(static_cast<std::size_t>(argument_count));
    for (int index = 0; index < argument_count; ++index) {
        argument_view.push_back(raw_arguments[index]);
    }
    ServiceArguments parsed;
    const auto view = std::span<const wchar_t* const>(argument_view);
    if (!parse_arguments(view, parsed) || !parsed.service_mode) {
        return aegra::base::Result<ServiceArguments>::failure(
            {aegra::base::ErrorCode::kInvalidArgument, "Service command line is invalid"});
    }
    return aegra::base::Result<ServiceArguments>::success(std::move(parsed));
}

DWORD WINAPI service_control_handler(const DWORD control, DWORD, LPVOID,
                                     const LPVOID context) noexcept {
    if (control == SERVICE_CONTROL_STOP && context != nullptr) {
        static_cast<service::WindowsServiceScmHost*>(context)->request_stop();
    }
    return NO_ERROR;
}

VOID WINAPI service_main_entry(DWORD, LPWSTR*) noexcept {
    try {
        auto arguments = parse_process_arguments();
        if (!arguments)
            return;

        service::WindowsServiceScmHost scm_host;
        const SERVICE_STATUS_HANDLE handle = ::RegisterServiceCtrlHandlerExW(
            const_cast<LPWSTR>(kServiceName.data()), service_control_handler, &scm_host);
        if (handle == nullptr)
            return;

        WindowsServiceStatusReporter reporter(handle);
        (void)scm_host.run(reporter, [parsed = std::move(arguments).value()](
                                         const aegra::base::CancellationToken& cancellation) {
            auto runtime = create_runtime(parsed);
            if (!runtime)
                return aegra::base::Result<void>::failure(runtime.error());
            auto listener = create_listener(parsed);
            if (!listener)
                return aegra::base::Result<void>::failure(listener.error());
            if (runtime.value().runtime.logger != nullptr) {
                runtime.value().runtime.logger->write(service::ServiceLogLevel::kInfo,
                                                      "service.listener_started",
                                                      "status=listening mode=service");
            }
            service::ServiceHostOptions options;
            options.once = parsed.once;
            return service::run_service_host(*listener.value(), runtime.value().runtime, options,
                                             cancellation);
        });
    } catch (...) {
        return;
    }
}

[[nodiscard]] ServiceExitCode run_service(const std::span<const wchar_t* const> arguments) {
    ServiceArguments parsed;
    if (!parse_arguments(arguments, parsed))
        return ServiceExitCode::kInvalidArguments;
    if (parsed.service_mode) {
        SERVICE_TABLE_ENTRYW dispatch_table[] = {
            {const_cast<LPWSTR>(kServiceName.data()), service_main_entry}, {nullptr, nullptr}};
        return ::StartServiceCtrlDispatcherW(dispatch_table) ? ServiceExitCode::kSucceeded
                                                             : ServiceExitCode::kHostFailure;
    }
    auto runtime = create_runtime(parsed);
    if (!runtime)
        return ServiceExitCode::kHostFailure;
    auto listener = create_listener(parsed);
    if (!listener)
        return ServiceExitCode::kHostFailure;
    if (runtime.value().runtime.logger != nullptr) {
        runtime.value().runtime.logger->write(service::ServiceLogLevel::kInfo,
                                              "service.listener_started", "status=listening");
    }
    return parsed.once ? run_once(*listener.value(), runtime.value().runtime)
                       : run_forever(*listener.value(), runtime.value().runtime);
}

} // namespace

int wmain(const int argument_count, const wchar_t* const* arguments) noexcept {
    try {
        return static_cast<int>(run_service({arguments, static_cast<std::size_t>(argument_count)}));
    } catch (...) {
        return static_cast<int>(ServiceExitCode::kHostFailure);
    }
}
