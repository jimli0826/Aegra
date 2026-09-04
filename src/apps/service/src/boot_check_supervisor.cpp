#include "aegra/apps/service/boot_check_supervisor.h"

#include "aegra/contracts/boot_check_job.h"

#include "worker_job_service_detail.h"

#include "aegra/apps/service/service_host.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/personal_repository/chain_graph.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
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

/// Cancels the wait after the run budget so a hung host is terminated.
class RunDeadline final {
  public:
    RunDeadline()
        : watchdog_([this](const std::stop_token stopped) {
              std::unique_lock lock(mutex_);
              changed_.wait_for(lock, stopped, kRunBudget, [] { return false; });
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
    Impl(Options run_options, ports::IProcessLauncher& process_launcher,
         ports::IControlPlaneDatabase& database, ports::IRepositoryStorageFactory& storage,
         ports::IClock& clock_source, IServiceLog* const service_logger)
        : options(std::move(run_options)), launcher(process_launcher), control_plane(database),
          storage_factory(storage), clock(clock_source), logger(service_logger) {}

    Options options;
    ports::IProcessLauncher& launcher;
    ports::IControlPlaneDatabase& control_plane;
    ports::IRepositoryStorageFactory& storage_factory;
    ports::IClock& clock;
    IServiceLog* logger{nullptr};

    mutable std::mutex mutex;
    bool stopping{false};
    bool scavenging{false};
    std::string active_job_id;
    std::optional<std::pair<std::string, BootCheckRunResult>> finished;
    std::function<void()> completion_observer;
    std::jthread runner;
    std::jthread scavenger;
    RunDeadline* active_deadline{nullptr};

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
                  {"source_refs", std::move(source_refs)},
                  {"credential_refs", std::move(credential_refs)},
                  {"job_directory", path_to_utf8(job_directory)},
                  {"deadline_utc_ms", deadline}};
        return base::Result<std::string>::success(root.dump());
    }

    [[nodiscard]] BootCheckRunResult execute(const BootCheckDispatch& dispatch,
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
        ports::ProcessLaunchRequest launch;
        launch.executable_path = path_to_utf8(options.host_executable_path);
        launch.arguments = {"--request", path_to_utf8(request_path)};
        launch.capture_output = true;
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
            active_deadline = &deadline;
        }
        std::stop_callback stop_forward(stopped, [&deadline] { deadline.cancel(); });
        auto exited = launcher.wait(launched.value().pid, deadline.token());
        {
            std::lock_guard lock(mutex);
            active_deadline = nullptr;
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

    void run(const BootCheckDispatch dispatch, const std::stop_token stopped) {
        auto result = execute(dispatch, stopped);
        write_log(logger, result.succeeded ? ServiceLogLevel::kInfo : ServiceLogLevel::kWarning,
                  "post_backup.boot_check_finished",
                  "BootCheck " + dispatch.boot_check_job_id +
                      " finished; message=" + result.message_code);
        std::function<void()> observer;
        {
            std::lock_guard lock(mutex);
            finished.emplace(dispatch.boot_check_job_id, std::move(result));
            active_job_id.clear();
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
        ports::ProcessLaunchRequest launch;
        launch.executable_path = path_to_utf8(state->options.host_executable_path);
        launch.arguments = {"--scavenge"};
        auto launched = state->launcher.launch(launch);
        if (launched) {
            RunDeadline deadline;
            std::stop_callback stop_forward(stopped, [&deadline] { deadline.cancel(); });
            auto exited = state->launcher.wait(launched.value().pid, deadline.token());
            if (!exited) {
                (void)state->launcher.terminate(launched.value().pid);
            }
            write_log(state->logger, ServiceLogLevel::kInfo, "post_backup.boot_check_scavenged",
                      exited && exited.value().exit_code == 0 ? "status=clean"
                                                              : "status=incomplete");
        } else {
            write_log(state->logger, ServiceLogLevel::kWarning,
                      "post_backup.boot_check_scavenge_failed", "status=launch_failed");
        }
        std::lock_guard lock(state->mutex);
        state->scavenging = false;
    });
}

bool BootCheckSupervisor::try_start(const BootCheckDispatch& dispatch) {
    if (!available() || dispatch.boot_check_job_id.empty()) {
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->scavenging || !impl_->active_job_id.empty() ||
        impl_->finished.has_value()) {
        return false;
    }
    // The finished previous thread is joined here (active_job_id empty implies
    // the runner body completed or never started).
    impl_->runner = std::jthread([state = impl_.get(), dispatch](const std::stop_token stopped) {
        state->run(dispatch, stopped);
    });
    impl_->active_job_id = dispatch.boot_check_job_id;
    return true;
}

std::optional<BootCheckRunResult>
BootCheckSupervisor::take_result(const std::string_view boot_check_job_id) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->finished || impl_->finished->first != boot_check_job_id) {
        return std::nullopt;
    }
    auto result = std::move(impl_->finished->second);
    impl_->finished.reset();
    return result;
}

bool BootCheckSupervisor::is_tracking(const std::string_view boot_check_job_id) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->active_job_id == boot_check_job_id ||
           (impl_->finished && impl_->finished->first == boot_check_job_id);
}

void BootCheckSupervisor::shutdown() noexcept {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) {
            return;
        }
        impl_->stopping = true;
        if (impl_->active_deadline != nullptr) {
            impl_->active_deadline->cancel();
        }
    }
    impl_->runner.request_stop();
    impl_->scavenger.request_stop();
    if (impl_->runner.joinable()) {
        impl_->runner.join();
    }
    if (impl_->scavenger.joinable()) {
        impl_->scavenger.join();
    }
}

} // namespace aegra::apps::service
