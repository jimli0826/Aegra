#include "aegra/adapters/sqlite/sqlite_control_plane.h"
#include "aegra/adapters/storage_local/local_object_storage.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/adapters/windows_process/windows_process_launcher.h"
#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/application/connected_repository_query.h"
#include "aegra/application/personal_repository_query.h"
#include "aegra/application/repository_connection_service.h"
#include "aegra/application/source_inventory_query.h"
#include "aegra/apps/service/service_host.h"
#include "aegra/apps/service/service_protocol.h"
#include "aegra/apps/service/service_security_host.h"
#include "aegra/apps/service/windows_service_scm_host.h"
#include "aegra/apps/service/worker_job_service.h"
#include "aegra/apps/service/worker_supervisor.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
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
namespace windows_ipc = aegra::adapters::windows_ipc;
namespace windows_process = aegra::adapters::windows_process;
namespace windows_system = aegra::adapters::windows_system;

constexpr std::wstring_view kServiceName = L"AegraService";

enum class ServiceExitCode : int {
    kSucceeded = 0,
    kInvalidArguments = 20,
    kHostFailure = 21,
};

struct ServiceArguments final {
    bool once{false};
    bool service_mode{false};
    std::string pipe_name{"control"};
    std::optional<std::filesystem::path> repository_root;
    std::optional<std::filesystem::path> data_dir;
    std::optional<std::filesystem::path> worker_path;
};

struct RuntimeComponents final {
    std::unique_ptr<windows_system::WindowsSystemClock> clock;
    std::unique_ptr<windows_system::WindowsCryptographicRandom> random;
    std::unique_ptr<windows_disk::WindowsSourceInventory> source_inventory;
    std::unique_ptr<storage_local::LocalRepositoryStorageFactory> storage_factory;
    std::unique_ptr<windows_process::WindowsProcessLauncher> process_launcher;
    std::unique_ptr<storage_local::LocalObjectStorage> storage;
    std::unique_ptr<aegra::application::PersonalRepositoryQuery> repository_query;
    std::unique_ptr<sqlite::SqliteControlPlaneDatabase> control_plane;
    std::unique_ptr<aegra::application::SourceInventoryQuery> source_query;
    std::unique_ptr<aegra::application::RepositoryConnectionService> connection_service;
    std::unique_ptr<aegra::application::ConnectedRepositoryQuery> connected_query;
    std::unique_ptr<service::WorkerSupervisor> supervisor;
    std::unique_ptr<service::WorkerJobService> worker_jobs;
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
        } else {
            return false;
        }
    }
    const auto absolute = [](const auto& value) { return !value || value->is_absolute(); };
    return absolute(result.repository_root) && absolute(result.data_dir) &&
           absolute(result.worker_path);
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
resolve_worker_path(const ServiceArguments& arguments) {
    if (arguments.worker_path) {
        return aegra::base::Result<std::filesystem::path>::success(*arguments.worker_path);
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
                                                               L"aegra_personal_worker.exe");
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
    auto interrupted =
        unit.value()->jobs().mark_active_as_interrupted(components.clock->now_utc_ms(), {});
    if (!interrupted) {
        unit.value()->rollback();
        return aegra::base::Result<void>::failure(interrupted.error());
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

[[nodiscard]] std::vector<std::string> runtime_capabilities() {
    std::vector<std::string> capabilities{
        "backup.start",    "job.cancel",   "job.list",         "repository.connection",
        "repository.list", "service.info", "source.inventory",
    };
    std::ranges::sort(capabilities);
    return capabilities;
}

[[nodiscard]] aegra::base::Result<RuntimeComponents>
create_runtime(const ServiceArguments& arguments) {
    RuntimeComponents components;
    components.clock = std::make_unique<windows_system::WindowsSystemClock>();
    components.random = std::make_unique<windows_system::WindowsCryptographicRandom>();
    components.source_inventory = std::make_unique<windows_disk::WindowsSourceInventory>();
    components.storage_factory = std::make_unique<storage_local::LocalRepositoryStorageFactory>();
    components.process_launcher = std::make_unique<windows_process::WindowsProcessLauncher>();

    auto data_dir = resolve_data_dir(arguments);
    auto worker_path = resolve_worker_path(arguments);
    if (!data_dir || !worker_path) {
        return aegra::base::Result<RuntimeComponents>::failure(!data_dir ? data_dir.error()
                                                                         : worker_path.error());
    }
    auto worker_path_utf8 = path_to_utf8(worker_path.value());
    if (!worker_path_utf8) {
        return aegra::base::Result<RuntimeComponents>::failure(worker_path_utf8.error());
    }
    auto control_plane = open_control_plane(data_dir.value(), components);
    auto repository = open_legacy_repository(arguments, components);
    if (!control_plane || !repository) {
        return aegra::base::Result<RuntimeComponents>::failure(
            !control_plane ? control_plane.error() : repository.error());
    }

    components.source_query =
        std::make_unique<aegra::application::SourceInventoryQuery>(*components.source_inventory);
    components.connection_service =
        std::make_unique<aegra::application::RepositoryConnectionService>(
            *components.control_plane, *components.storage_factory, *components.clock,
            *components.random);
    components.connected_query = std::make_unique<aegra::application::ConnectedRepositoryQuery>(
        *components.control_plane, *components.storage_factory);
    service::WorkerSupervisorConfig supervisor_config;
    supervisor_config.worker_executable_path = std::move(worker_path_utf8).value();
    components.supervisor = std::make_unique<service::WorkerSupervisor>(
        std::move(supervisor_config), *components.process_launcher, *components.control_plane,
        *components.clock, *components.random, service::SupervisorProgressCallback{},
        service::SupervisorCompletionCallback{});
    components.worker_jobs = std::make_unique<service::WorkerJobService>(
        *components.source_query, *components.control_plane, *components.supervisor,
        *components.clock, *components.random);
    components.runtime = {
        .service_version = AEGRA_APPLICATION_VERSION,
        .capabilities = runtime_capabilities(),
        .repository_query = components.repository_query.get(),
        .connected_repository_query = components.connected_query.get(),
        .repository_connections = components.connection_service.get(),
        .source_inventory = components.source_query.get(),
        .worker_jobs = components.worker_jobs.get(),
        .control_plane = components.control_plane.get(),
    };
    return aegra::base::Result<RuntimeComponents>::success(std::move(components));
}

[[nodiscard]] aegra::base::Result<std::unique_ptr<windows_ipc::WindowsNamedPipeListener>>
create_listener(const ServiceArguments& arguments) {
    const auto acl = arguments.service_mode
                         ? windows_ipc::WindowsNamedPipeAclProfile::kServiceLocalControl
                         : windows_ipc::WindowsNamedPipeAclProfile::kProcessDefault;
    return windows_ipc::WindowsNamedPipeListener::create(
        {arguments.pipe_name, static_cast<std::uint32_t>(service::kMaximumServiceFrameBytes), acl});
}

[[nodiscard]] ServiceExitCode run_once(windows_ipc::WindowsNamedPipeListener& listener,
                                       const service::ServiceRuntimeInfo& runtime) {
    auto channel = listener.accept({});
    if (!channel)
        return ServiceExitCode::kHostFailure;
    auto result = service::run_service_session(*channel.value(), runtime, {}, 1);
    return result ? ServiceExitCode::kSucceeded : ServiceExitCode::kHostFailure;
}

[[nodiscard]] ServiceExitCode run_forever(windows_ipc::WindowsNamedPipeListener& listener,
                                          const service::ServiceRuntimeInfo& runtime) {
    for (;;) {
        auto channel = listener.accept({});
        if (!channel)
            return ServiceExitCode::kHostFailure;
        (void)service::run_service_session(*channel.value(), runtime, {});
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
            service::ServiceSecurityHostOptions options;
            options.once = parsed.once;
            return service::run_authorized_service_host(*listener.value(), runtime.value().runtime,
                                                        options, cancellation);
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
