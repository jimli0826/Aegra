#include "aegra/apps/service/boot_check_supervisor.h"

#include "aegra/contracts/boot_check_job.h"

#include "worker_job_service_detail.h"

#include "aegra/apps/service/service_host.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/personal_repository/chain_graph.h"

#include <Windows.h>

#include <aclapi.h>
#include <wtsapi32.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

using Json = nlohmann::json;
using worker_job_detail::path_to_utf8;
using worker_job_detail::resolve_archive_absolute_path;

constexpr auto kRunBudget = std::chrono::minutes(45);
// A capability probe creates one throwaway VM at most; a hung probe must not
// hold the refresh pipeline for the full job budget.
constexpr auto kProbeBudget = std::chrono::minutes(5);
// Catalog publish may still be settling right after backup completion (no verify
// runtime in between when verify is disabled); bounded wait for the tip entry.
constexpr unsigned kCatalogSettleAttempts = 12;
constexpr auto kCatalogSettleInterval = std::chrono::seconds(5);
constexpr const char* kDispatchFailed = "post_backup.boot_check_dispatch_failed";
constexpr const char* kHostFailed = "post_backup.boot_check_host_failed";
constexpr const char* kTimedOut = "post_backup.boot_check_timeout";

void write_log(IServiceLog* const logger, const ServiceLogLevel level, const std::string_view code,
               const std::string_view detail) noexcept {
    if (logger != nullptr) {
        logger->write(level, code, detail);
    }
}

/// Grants the interactive user full control (inheritable) on the BootCheck tree
/// so a host launched under that user's token can read staged requests and write
/// job directories. Best-effort: no active user, or an ACL failure, simply leaves
/// the tree LocalSystem-only (the host then falls back to a System launch).
void grant_active_user_access(const std::filesystem::path& directory) noexcept {
    HANDLE raw_token = nullptr;
    const DWORD console = WTSGetActiveConsoleSessionId();
    if (console == 0xFFFFFFFFU || WTSQueryUserToken(console, &raw_token) == FALSE) {
        return;
    }
    struct TokenGuard final {
        HANDLE value;
        ~TokenGuard() {
            if (value != nullptr) {
                CloseHandle(value);
            }
        }
    } token_guard{raw_token};
    DWORD size = 0;
    GetTokenInformation(raw_token, TokenUser, nullptr, 0, &size);
    if (size == 0) {
        return;
    }
    std::vector<std::byte> buffer(size);
    if (GetTokenInformation(raw_token, TokenUser, buffer.data(), size, &size) == FALSE) {
        return;
    }
    PSID user_sid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;

    PACL existing_dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto path = directory.wstring();
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                              nullptr, &existing_dacl, nullptr, &descriptor) != ERROR_SUCCESS) {
        return;
    }
    struct DescriptorGuard final {
        PSECURITY_DESCRIPTOR value;
        ~DescriptorGuard() {
            if (value != nullptr) {
                LocalFree(value);
            }
        }
    } descriptor_guard{descriptor};

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(user_sid);

    PACL merged_dacl = nullptr;
    if (SetEntriesInAclW(1, &access, existing_dacl, &merged_dacl) != ERROR_SUCCESS) {
        return;
    }
    (void)SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                nullptr, merged_dacl, nullptr);
    LocalFree(merged_dacl);
}

/// User-token launches get a fresh profile environment (no Service inherit), so
/// AEGRA_DATA_DIR must be stamped and the interactive user must be able to write
/// job files and task logs under the Service data directory.
void apply_boot_check_host_environment(ports::ProcessLaunchRequest& launch,
                                       const std::filesystem::path& data_directory,
                                       const bool as_active_user) {
    launch.environment_overrides.push_back(
        ports::ProcessEnvironmentVariable{"AEGRA_DATA_DIR", path_to_utf8(data_directory)});
    if (!as_active_user) {
        return;
    }
    grant_active_user_access(data_directory / L"bootcheck");
    std::error_code error;
    const auto log_directory = data_directory / L"logs" / L"bootcheck";
    std::filesystem::create_directories(log_directory, error);
    if (!error) {
        grant_active_user_access(log_directory);
    }
}

/// tip -> Full chain of catalog entries for a volume recovery point (base-first).
[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
resolve_volume_chain(ports::IControlPlaneDatabase& control_plane,
                     ports::IRepositoryStorageFactory& storage_factory,
                     const std::string& connection_id, const std::string& recovery_point_id,
                     const base::CancellationToken cancellation) {
    using Outcome = base::Result<std::vector<personal_repository::CatalogEntry>>;
    auto repository = control_plane.get_repository_connection(connection_id, cancellation);
    if (!repository) {
        return Outcome::failure(repository.error());
    }
    if (!repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return Outcome::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    auto storage = storage_factory.open(repository.value()->locator, cancellation);
    if (!storage) {
        return Outcome::failure(storage.error());
    }
    personal_repository::RepositoryCatalogScanner scanner(storage.value()->reader(),
                                                          storage.value()->enumerator());
    auto loaded = scanner.load_entries(cancellation);
    if (!loaded) {
        return Outcome::failure(loaded.error());
    }
    auto graph = personal_repository::RecoveryPointGraph::build(std::move(loaded).value().entries);
    if (!graph) {
        return Outcome::failure(graph.error());
    }
    auto chain = graph.value().resolve_chain(recovery_point_id);
    if (!chain) {
        return Outcome::failure(chain.error());
    }
    if (chain.value().empty() || chain.value().back().file_uuid != recovery_point_id) {
        return Outcome::failure({base::ErrorCode::kConflict, "recovery point chain is incomplete"});
    }
    return Outcome::success(std::move(chain).value());
}

[[nodiscard]] base::Result<void> write_request_file(const std::filesystem::path& path,
                                                    const std::string& content) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "boot check request staging failed"});
    }
    DWORD written = 0;
    const BOOL ok =
        WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    const BOOL flushed = ok ? FlushFileBuffers(file) : FALSE;
    CloseHandle(file);
    if (!ok || written != content.size() || !flushed) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "boot check request staging failed"});
    }
    return base::Result<void>::success();
}

struct InspectOutcome final {
    bool available{false};
    std::string message_code{"bootcheck.provider_unavailable"};
};

/// Extracts an `--inspect` result from the host's captured stdout. Any launch,
/// parse, or shape failure keeps the unavailable default.
[[nodiscard]] InspectOutcome parse_inspect_response(const std::string& output) {
    InspectOutcome outcome;
    const auto begin = output.find('{');
    const auto end = output.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end <= begin) {
        return outcome;
    }
    try {
        const auto root = Json::parse(output.substr(begin, end - begin + 1));
        if (!root.is_object() || root.value("schema_version", 0) != 1 ||
            root.value("kind", std::string{}) != "inspect") {
            return outcome;
        }
        outcome.available = root.value("available", false);
        if (outcome.available) {
            outcome.message_code.clear();
        } else if (const auto code = root.value("message_code", std::string{}); !code.empty()) {
            outcome.message_code = code;
        }
    } catch (const std::exception&) {
        outcome = InspectOutcome{};
    }
    return outcome;
}

/// Extracts the terminal outcome from the host's captured stdout (one
/// WorkerResponse JSON line, possibly surrounded by stray output).
[[nodiscard]] BootCheckRunResult parse_host_response(const std::string& output) {
    BootCheckRunResult result;
    result.message_code = kHostFailed;
    const auto begin = output.find('{');
    const auto end = output.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end <= begin) {
        return result;
    }
    try {
        const auto root = Json::parse(output.substr(begin, end - begin + 1));
        if (!root.is_object() || root.value("schema_version", 0) != 1 ||
            root.value("kind", 0) != 1 || !root.contains("task_result") ||
            !root["task_result"].is_object()) {
            return result;
        }
        const auto& task = root["task_result"];
        const auto outcome = task.value("outcome", 0);
        result.succeeded = outcome == 1 || outcome == 2;
        result.message_code = task.value("message_code", std::string(kHostFailed));
        if (result.message_code.empty()) {
            result.message_code = kHostFailed;
        }
    } catch (const std::exception&) {
        result.succeeded = false;
        result.message_code = kHostFailed;
    }
    return result;
}

/// Cancels the wait after the given budget so a hung host is terminated.
class RunDeadline final {
  public:
    explicit RunDeadline(const std::chrono::minutes budget = kRunBudget)
        : watchdog_([this, budget](const std::stop_token stopped) {
              std::unique_lock lock(mutex_);
              changed_.wait_for(lock, stopped, budget, [] { return false; });
              if (!stopped.stop_requested()) {
                  cancellation_.request_stop();
              }
          }) {}

    ~RunDeadline() {
        watchdog_.request_stop();
        changed_.notify_all();
    }

    RunDeadline(const RunDeadline&) = delete;
    RunDeadline& operator=(const RunDeadline&) = delete;

    void cancel() noexcept { cancellation_.request_stop(); }
    [[nodiscard]] base::CancellationToken token() const noexcept {
        return cancellation_.get_token();
    }

  private:
    base::CancellationSource cancellation_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::jthread watchdog_;
};

} // namespace

struct BootCheckSupervisor::Impl final {
    struct RunEntry final {
        std::uint32_t memory_mib{0};
        std::optional<BootCheckRunResult> result;
        std::jthread runner;
        RunDeadline* deadline{nullptr};
    };

    Impl(Options run_options, ports::IProcessLauncher& process_launcher,
         ports::IControlPlaneDatabase& database, ports::IRepositoryStorageFactory& storage,
         ports::IClock& clock_source, IServiceLog* const service_logger)
        : options(std::move(run_options)), launcher(process_launcher), control_plane(database),
          storage_factory(storage), clock(clock_source), logger(service_logger) {
        virtualbox_status.hypervisor = contracts::BootCheckHypervisor::kVirtualBox;
        virtualbox_status.installed = options.virtualbox_installed;
        hyperv_status.hypervisor = contracts::BootCheckHypervisor::kHyperV;
        hyperv_status.installed = options.hyperv_installed;
    }

    Options options;
    ports::IProcessLauncher& launcher;
    ports::IControlPlaneDatabase& control_plane;
    ports::IRepositoryStorageFactory& storage_factory;
    ports::IClock& clock;
    IServiceLog* logger{nullptr};

    mutable std::mutex mutex;
    bool stopping{false};
    bool scavenging{false};
    bool probing{false};
    contracts::BootCheckHypervisorStatus virtualbox_status;
    contracts::BootCheckHypervisorStatus hyperv_status;
    std::unordered_map<std::string, std::unique_ptr<RunEntry>> runs;
    std::function<void()> completion_observer;
    std::jthread scavenger;
    std::jthread prober;

    /// Runs one `--inspect` probe and publishes the outcome into `status`.
    void probe_one(const char* const hypervisor_name,
                   contracts::BootCheckHypervisorStatus& status, const std::stop_token stopped) {
        ports::ProcessLaunchRequest launch;
        launch.executable_path = path_to_utf8(options.host_executable_path);
        launch.arguments = {"--inspect", hypervisor_name};
        launch.capture_output = true;
        apply_boot_check_host_environment(launch, options.data_directory, false);
        InspectOutcome outcome;
        auto launched = launcher.launch(launch);
        if (launched) {
            RunDeadline deadline(kProbeBudget);
            std::stop_callback stop_forward(stopped, [&deadline] { deadline.cancel(); });
            auto exited = launcher.wait(launched.value().pid, deadline.token());
            if (!exited) {
                (void)launcher.terminate(launched.value().pid);
            } else {
                outcome = parse_inspect_response(exited.value().output);
            }
        }
        const auto now = clock.now_utc_ms();
        {
            std::lock_guard lock(mutex);
            status.probe_state = contracts::BootCheckProbeState::kProbed;
            status.available = outcome.available;
            status.message_code = outcome.message_code;
            status.checked_utc_ms = now < 0 ? 0 : static_cast<std::uint64_t>(now);
        }
        write_log(logger, outcome.available ? ServiceLogLevel::kInfo : ServiceLogLevel::kWarning,
                  "boot_check.hypervisor_probed",
                  std::string("hypervisor=") + hypervisor_name +
                      "; available=" + (outcome.available ? "true" : "false") +
                      (outcome.message_code.empty() ? "" : "; message=" + outcome.message_code));
    }

    [[nodiscard]] base::Result<std::string> build_request_json(const BootCheckDispatch& dispatch) {
        auto chain =
            resolve_volume_chain(control_plane, storage_factory, dispatch.repository_connection_id,
                                 dispatch.recovery_point_id, {});
        if (!chain) {
            return base::Result<std::string>::failure(chain.error());
        }
        auto repository =
            control_plane.get_repository_connection(dispatch.repository_connection_id, {});
        if (!repository || !repository.value()) {
            return base::Result<std::string>::failure(
                {base::ErrorCode::kNotFound, "repository connection was not found"});
        }
        std::string credential;
        auto schedule = control_plane.get_schedule(dispatch.schedule_id, {});
        if (schedule && schedule.value() && schedule.value()->encryption_enabled) {
            credential = schedule.value()->archive_password_protected;
        }
        Json source_refs = Json::array();
        Json credential_refs = Json::array();
        for (const auto& layer : chain.value()) {
            auto path =
                resolve_archive_absolute_path(repository.value()->locator, layer.archive_main_key);
            if (!path) {
                return base::Result<std::string>::failure(path.error());
            }
            source_refs.push_back(std::move(path).value());
            credential_refs.push_back(credential);
        }
        const auto now = clock.now_utc_ms();
        const auto deadline =
            now < 0 ? 0ULL
                    : static_cast<std::uint64_t>(now) +
                          static_cast<std::uint64_t>(
                              std::chrono::duration_cast<std::chrono::milliseconds>(kRunBudget)
                                  .count());
        const auto job_directory =
            options.data_directory / L"bootcheck" / L"jobs" / dispatch.boot_check_job_id;
        Json root{{"schema_version", contracts::kBootCheckJobSchemaVersion},
                  {"job_id", dispatch.boot_check_job_id},
                  {"trace_id", "trace-" + dispatch.boot_check_job_id},
                  {"hypervisor", static_cast<std::uint8_t>(dispatch.hypervisor)},
                  {"cpu_count", dispatch.cpu_count},
                  {"memory_mib", dispatch.memory_mib},
                  {"source_refs", std::move(source_refs)},
                  {"credential_refs", std::move(credential_refs)},
                  {"job_directory", path_to_utf8(job_directory)},
                  {"deadline_utc_ms", deadline}};
        return base::Result<std::string>::success(root.dump());
    }

    [[nodiscard]] BootCheckRunResult execute(const BootCheckDispatch& dispatch, RunEntry& entry,
                                             const std::stop_token stopped) {
        auto request_json = build_request_json(dispatch);
        for (unsigned attempt = 1;
             !request_json && request_json.error().code == base::ErrorCode::kNotFound &&
             attempt < kCatalogSettleAttempts && !stopped.stop_requested();
             ++attempt) {
            std::mutex wait_mutex;
            std::condition_variable_any changed;
            std::unique_lock lock(wait_mutex);
            changed.wait_for(lock, stopped, kCatalogSettleInterval, [] { return false; });
            if (stopped.stop_requested()) {
                break;
            }
            request_json = build_request_json(dispatch);
        }
        if (!request_json) {
            write_log(logger, ServiceLogLevel::kError, kDispatchFailed,
                      "BootCheck dispatch failed for " + dispatch.boot_check_job_id + "; error=" +
                          std::string(base::error_code_name(request_json.error().code)) +
                          (request_json.error().message.empty()
                               ? ""
                               : "; detail=" + request_json.error().message));
            return {false, kDispatchFailed};
        }
        const auto staging_dir = options.data_directory / L"bootcheck" / L"staging";
        std::error_code ec;
        std::filesystem::create_directories(staging_dir, ec);
        const auto request_path = staging_dir / (dispatch.boot_check_job_id + ".request.json");
        if (ec || !write_request_file(request_path, request_json.value())) {
            return {false, kDispatchFailed};
        }
        // VirtualBox registers per-user, so run the host under the logged-on
        // user's token to make the job VM appear in their VirtualBox Manager.
        // Hyper-V VMs are global (System launch keeps the required privileges).
        const bool user_visible =
            dispatch.hypervisor == contracts::BootCheckHypervisor::kVirtualBox;
        ports::ProcessLaunchRequest launch;
        launch.executable_path = path_to_utf8(options.host_executable_path);
        launch.arguments = {"--request", path_to_utf8(request_path)};
        launch.capture_output = true;
        launch.run_as_active_user = user_visible;
        apply_boot_check_host_environment(launch, options.data_directory, user_visible);
        auto launched = launcher.launch(launch);
        if (!launched) {
            std::filesystem::remove(request_path, ec);
            write_log(logger, ServiceLogLevel::kError, kDispatchFailed,
                      "BootCheck host launch failed for " + dispatch.boot_check_job_id);
            return {false, kDispatchFailed};
        }
        RunDeadline deadline;
        {
            std::lock_guard lock(mutex);
            entry.deadline = &deadline;
        }
        std::stop_callback stop_forward(stopped, [&deadline] { deadline.cancel(); });
        auto exited = launcher.wait(launched.value().pid, deadline.token());
        {
            std::lock_guard lock(mutex);
            entry.deadline = nullptr;
        }
        std::filesystem::remove(request_path, ec);
        if (!exited) {
            (void)launcher.terminate(launched.value().pid);
            write_log(logger, ServiceLogLevel::kError, kTimedOut,
                      "BootCheck host exceeded the run budget for " + dispatch.boot_check_job_id);
            return {false, kTimedOut};
        }
        return parse_host_response(exited.value().output);
    }

    void run(const BootCheckDispatch dispatch, RunEntry& entry, const std::stop_token stopped) {
        auto result = execute(dispatch, entry, stopped);
        write_log(logger, result.succeeded ? ServiceLogLevel::kInfo : ServiceLogLevel::kWarning,
                  "post_backup.boot_check_finished",
                  "BootCheck " + dispatch.boot_check_job_id +
                      " finished; message=" + result.message_code);
        std::function<void()> observer;
        {
            std::lock_guard lock(mutex);
            entry.result.emplace(std::move(result));
            observer = completion_observer;
        }
        if (observer) {
            observer();
        }
    }
};

BootCheckSupervisor::BootCheckSupervisor(Options options, ports::IProcessLauncher& launcher,
                                         ports::IControlPlaneDatabase& control_plane,
                                         ports::IRepositoryStorageFactory& storage_factory,
                                         ports::IClock& clock, IServiceLog* const logger)
    : impl_(std::make_unique<Impl>(std::move(options), launcher, control_plane, storage_factory,
                                   clock, logger)) {}

BootCheckSupervisor::~BootCheckSupervisor() { shutdown(); }

bool BootCheckSupervisor::available() const noexcept {
    return !impl_->options.host_executable_path.empty();
}

void BootCheckSupervisor::set_completion_observer(std::function<void()> observer) {
    std::lock_guard lock(impl_->mutex);
    impl_->completion_observer = std::move(observer);
}

void BootCheckSupervisor::begin_scavenge() {
    if (!available()) {
        return;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->scavenging || impl_->scavenger.joinable()) {
        return;
    }
    impl_->scavenging = true;
    impl_->scavenger = std::jthread([state = impl_.get()](const std::stop_token stopped) {
        // Two passes: a System pass clears isolated job directories/registries,
        // then an active-user pass clears orphaned VMs from the logged-on user's
        // own VirtualBox registry (user-visible job path). The user pass falls
        // back to System when nobody is logged on (a harmless no-op there).
        const auto run_scavenge = [state, &stopped](const bool as_active_user) {
            ports::ProcessLaunchRequest launch;
            launch.executable_path = path_to_utf8(state->options.host_executable_path);
            launch.arguments = {"--scavenge"};
            launch.run_as_active_user = as_active_user;
            apply_boot_check_host_environment(launch, state->options.data_directory, as_active_user);
            auto launched = state->launcher.launch(launch);
            if (!launched) {
                write_log(state->logger, ServiceLogLevel::kWarning,
                          "post_backup.boot_check_scavenge_failed", "status=launch_failed");
                return;
            }
            RunDeadline deadline;
            std::stop_callback stop_forward(stopped, [&deadline] { deadline.cancel(); });
            auto exited = state->launcher.wait(launched.value().pid, deadline.token());
            if (!exited) {
                (void)state->launcher.terminate(launched.value().pid);
            }
            write_log(state->logger, ServiceLogLevel::kInfo, "post_backup.boot_check_scavenged",
                      std::string(as_active_user ? "scope=user " : "scope=system ") +
                          (exited && exited.value().exit_code == 0 ? "status=clean"
                                                                   : "status=incomplete"));
        };
        run_scavenge(false);
        if (!stopped.stop_requested()) {
            run_scavenge(true);
        }
        std::lock_guard lock(state->mutex);
        state->scavenging = false;
    });
}

void BootCheckSupervisor::begin_hypervisor_probe() {
    if (!available()) {
        return;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->probing ||
        (!impl_->options.virtualbox_installed && !impl_->options.hyperv_installed)) {
        return;
    }
    impl_->probing = true;
    if (impl_->options.virtualbox_installed) {
        impl_->virtualbox_status.probe_state = contracts::BootCheckProbeState::kProbing;
    }
    if (impl_->options.hyperv_installed) {
        impl_->hyperv_status.probe_state = contracts::BootCheckProbeState::kProbing;
    }
    // The previous pass has finished (probing was false), so this move-assign
    // joins an already-completed thread.
    impl_->prober = std::jthread([state = impl_.get()](const std::stop_token stopped) {
        if (state->options.virtualbox_installed && !stopped.stop_requested()) {
            state->probe_one("virtualbox", state->virtualbox_status, stopped);
        }
        if (state->options.hyperv_installed && !stopped.stop_requested()) {
            state->probe_one("hyperv", state->hyperv_status, stopped);
        }
        std::lock_guard lock(state->mutex);
        state->probing = false;
    });
}

contracts::BootCheckHypervisorStatusReport BootCheckSupervisor::hypervisor_status() const {
    std::lock_guard lock(impl_->mutex);
    contracts::BootCheckHypervisorStatusReport report;
    report.hypervisors = {impl_->virtualbox_status, impl_->hyperv_status};
    return report;
}

bool BootCheckSupervisor::try_start(const BootCheckDispatch& dispatch) {
    if (!available() || dispatch.boot_check_job_id.empty()) {
        return false;
    }
    auto settings = impl_->control_plane.get_service_settings({});
    if (!settings) {
        return false;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) == FALSE) {
        return false;
    }
    const auto total_mib = memory.ullTotalPhys / (1024ULL * 1024ULL);
    const auto available_mib = memory.ullAvailPhys / (1024ULL * 1024ULL);
    const auto reserve_mib = (std::max)(2ULL * 1024ULL, total_mib / 4ULL);
    const auto budget_mib = total_mib > reserve_mib ? total_mib - reserve_mib : 0;
    const auto memory_mib = settings.value().boot_check_memory_mib;
    const auto effective_concurrency = static_cast<std::uint32_t>((std::min)(
        static_cast<std::uint64_t>(settings.value().boot_check_concurrency),
        memory_mib == 0 ? 0ULL : budget_mib / memory_mib));
    if (effective_concurrency == 0 || available_mib < reserve_mib + memory_mib) {
        return false;
    }
    BootCheckDispatch configured = dispatch;
    configured.cpu_count = (std::min)(
        settings.value().boot_check_cpu_count,
        static_cast<std::uint32_t>((std::min)(
            static_cast<DWORD>(contracts::kMaximumBootCheckCpuCount),
            (std::max)(1UL, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)))));
    configured.memory_mib = memory_mib;
    std::lock_guard lock(impl_->mutex);
    const auto active = std::ranges::count_if(impl_->runs, [](const auto& item) {
        return !item.second->result.has_value();
    });
    std::uint64_t active_memory_mib = 0;
    for (const auto& [job_id, run] : impl_->runs) {
        (void)job_id;
        if (!run->result) {
            active_memory_mib += run->memory_mib;
        }
    }
    if (impl_->stopping || impl_->scavenging ||
        active >= static_cast<std::ptrdiff_t>(effective_concurrency) ||
        active_memory_mib + configured.memory_mib > budget_mib ||
        impl_->runs.contains(configured.boot_check_job_id)) {
        return false;
    }
    auto entry = std::make_unique<Impl::RunEntry>();
    entry->memory_mib = configured.memory_mib;
    auto* const entry_ptr = entry.get();
    impl_->runs.emplace(configured.boot_check_job_id, std::move(entry));
    entry_ptr->runner = std::jthread([state = impl_.get(), configured,
                                      entry_ptr](const std::stop_token stopped) {
        state->run(configured, *entry_ptr, stopped);
    });
    return true;
}

std::optional<BootCheckRunResult>
BootCheckSupervisor::take_result(const std::string_view boot_check_job_id) {
    std::unique_ptr<Impl::RunEntry> completed;
    std::unique_lock lock(impl_->mutex);
    const auto found = impl_->runs.find(std::string(boot_check_job_id));
    if (found == impl_->runs.end() || !found->second->result) {
        return std::nullopt;
    }
    auto result = std::move(*found->second->result);
    completed = std::move(found->second);
    impl_->runs.erase(found);
    lock.unlock();
    return result;
}

bool BootCheckSupervisor::is_tracking(const std::string_view boot_check_job_id) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->runs.contains(std::string(boot_check_job_id));
}

void BootCheckSupervisor::shutdown() noexcept {
    std::unordered_map<std::string, std::unique_ptr<Impl::RunEntry>> runs;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) {
            return;
        }
        impl_->stopping = true;
        for (auto& [job_id, run] : impl_->runs) {
            (void)job_id;
            if (run->deadline != nullptr) {
                run->deadline->cancel();
            }
            run->runner.request_stop();
        }
        runs.swap(impl_->runs);
    }
    impl_->scavenger.request_stop();
    impl_->prober.request_stop();
    runs.clear();
    if (impl_->scavenger.joinable()) {
        impl_->scavenger.join();
    }
    if (impl_->prober.joinable()) {
        impl_->prober.join();
    }
}

} // namespace aegra::apps::service
