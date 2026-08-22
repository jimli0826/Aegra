#include "aegra/apps/service/mount_supervisor.h"

#include "worker_job_service_detail.h"

#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/personal_repository/chain_graph.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/process_launcher.h"
#include "aegra/ports/random.h"
#include "aegra/ports/repository_storage.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

using Json = nlohmann::json;
using adapters::windows_ipc::WindowsNamedPipeListener;
using worker_job_detail::acknowledgement;
using worker_job_detail::path_to_utf8;
using worker_job_detail::random_id;
using worker_job_detail::resolve_archive_absolute_path;

struct LiveSession final {
    contracts::MountSessionSummary summary;
    std::string repository_connection_id;
    std::uint32_t source_disk_number{0};
    std::string idempotency_key;
    std::string request_fingerprint;
    std::string pipe_name;
    std::uint32_t host_pid{0};
    std::unique_ptr<WindowsNamedPipeListener> listener;
    std::unique_ptr<ports::IMessageChannel> channel;
    std::jthread waiter;
    std::atomic<bool> finished{false};
};

[[nodiscard]] std::uint64_t utc_now_ms(const ports::IClock& clock) noexcept {
    const auto now = clock.now_utc_ms();
    return now < 0 ? 0U : static_cast<std::uint64_t>(now);
}

[[nodiscard]] base::Result<std::string>
generate_pipe_name(ports::IRandomSource& random, const base::CancellationToken cancellation) {
    std::array<std::byte, 16> bytes{};
    if (auto filled = random.fill(bytes, cancellation); !filled) {
        return base::Result<std::string>::failure(filled.error());
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string result = "aegra-mount-";
    result.reserve(44);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        result.push_back(kHex[value >> 4U]);
        result.push_back(kHex[value & 0x0FU]);
    }
    return base::Result<std::string>::success(std::move(result));
}

[[nodiscard]] std::string mount_fingerprint(const contracts::MountRecoveryPointCommand& command) {
    std::string out = "mount|";
    out.append(command.repository_connection_id);
    out.push_back('|');
    out.append(command.recovery_point_id);
    out.push_back('|');
    out.append(std::to_string(command.source_disk_number));
    out.push_back('|');
    out.append(command.preferred_drive_letter.value_or(""));
    return out;
}

[[nodiscard]] base::Result<std::vector<personal_repository::CatalogEntry>>
resolve_chain(ports::IControlPlaneDatabase& control_plane,
              ports::IRepositoryStorageFactory& storage_factory,
              const std::string_view connection_id, const std::string_view recovery_point_id,
              const base::CancellationToken cancellation) {
    auto repository = control_plane.get_repository_connection(connection_id, cancellation);
    if (!repository) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            repository.error());
    }
    if (!repository.value() ||
        repository.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    auto storage = storage_factory.open(repository.value()->locator, cancellation);
    if (!storage) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            storage.error());
    }
    personal_repository::RepositoryCatalogScanner scanner(storage.value()->reader(),
                                                          storage.value()->enumerator());
    auto loaded = scanner.load_entries(cancellation);
    if (!loaded) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            loaded.error());
    }
    auto graph = personal_repository::RecoveryPointGraph::build(std::move(loaded).value().entries);
    if (!graph) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(graph.error());
    }
    auto chain = graph.value().resolve_chain(recovery_point_id);
    if (!chain) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(chain.error());
    }
    if (chain.value().empty() || chain.value().back().file_uuid != recovery_point_id) {
        return base::Result<std::vector<personal_repository::CatalogEntry>>::failure(
            {base::ErrorCode::kConflict, "recovery point chain is incomplete"});
    }
    return base::Result<std::vector<personal_repository::CatalogEntry>>::success(
        std::move(chain).value());
}

[[nodiscard]] base::Result<std::string>
build_mount_request_json(const contracts::MountRecoveryPointCommand& command,
                         const std::string& session_id, const std::string& content_kind,
                         const std::string& overlay_dir_utf8,
                         const std::vector<std::string>& absolute_paths) {
    if (absolute_paths.empty()) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "mount chain is empty"});
    }
    Json layers = Json::array();
    for (const auto& path : absolute_paths) {
        layers.push_back(Json{{"path", path}, {"password", command.archive_password}});
    }
    Json root{{"schema_version", 1},
              {"operation", "mount"},
              {"session_id", session_id},
              {"content_kind", content_kind},
              {"source_disk_number", command.source_disk_number},
              {"preferred_drive_letter", command.preferred_drive_letter.value_or("")},
              {"overlay_dir", overlay_dir_utf8},
              {"layers", std::move(layers)}};
    return base::Result<std::string>::success(root.dump());
}

[[nodiscard]] base::Result<Json> parse_host_event(const std::string& frame) {
    try {
        auto root = Json::parse(frame);
        if (!root.is_object() || root.value("schema_version", 0) != 1) {
            return base::Result<Json>::failure(
                {base::ErrorCode::kInvalidArgument, "mount host event is invalid"});
        }
        return base::Result<Json>::success(std::move(root));
    } catch (const std::exception&) {
        return base::Result<Json>::failure(
            {base::ErrorCode::kInvalidArgument, "mount host event is not valid JSON"});
    }
}

} // namespace

struct MountSupervisor::Impl final {
    MountSupervisorConfig config;
    ports::IProcessLauncher* launcher{nullptr};
    ports::IControlPlaneDatabase* control_plane{nullptr};
    ports::IRepositoryStorageFactory* storage_factory{nullptr};
    ports::IClock* clock{nullptr};
    ports::IRandomSource* random{nullptr};
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<LiveSession>> sessions_by_id;
    std::unordered_map<std::string, std::string> session_by_idempotency;
};

MountSupervisor::MountSupervisor(MountSupervisorConfig config, ports::IProcessLauncher& launcher,
                                 ports::IControlPlaneDatabase& control_plane,
                                 ports::IRepositoryStorageFactory& storage_factory,
                                 ports::IClock& clock, ports::IRandomSource& random)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    impl_->launcher = &launcher;
    impl_->control_plane = &control_plane;
    impl_->storage_factory = &storage_factory;
    impl_->clock = &clock;
    impl_->random = &random;
}

MountSupervisor::~MountSupervisor() {
    shutdown({});
}

base::Result<contracts::CommandAcknowledgement>
MountSupervisor::mount(const contracts::MountRecoveryPointCommand& command,
                       const std::string_view idempotency_key,
                       const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_mount_recovery_point_command(command); !valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    if (idempotency_key.empty()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kInvalidArgument, "mount command requires an idempotency key"});
    }
    if (impl_->config.mount_host_executable_path.empty()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "mount.host_unavailable"});
    }

    const auto fingerprint = mount_fingerprint(command);
    {
        std::lock_guard lock(impl_->mutex);
        if (const auto existing = impl_->session_by_idempotency.find(std::string(idempotency_key));
            existing != impl_->session_by_idempotency.end()) {
            const auto session = impl_->sessions_by_id.find(existing->second);
            if (session != impl_->sessions_by_id.end() &&
                session->second->request_fingerprint == fingerprint) {
                return base::Result<contracts::CommandAcknowledgement>::success(acknowledgement(
                    std::string(idempotency_key), contracts::CommandDisposition::kReplayed,
                    session->second->summary.session_id));
            }
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "mount idempotency key conflicts"});
        }
        for (const auto& [id, session] : impl_->sessions_by_id) {
            if (session->summary.recovery_point_id == command.recovery_point_id &&
                session->source_disk_number == command.source_disk_number &&
                session->repository_connection_id == command.repository_connection_id &&
                (session->summary.state == contracts::MountSessionState::kMounting ||
                 session->summary.state == contracts::MountSessionState::kMounted)) {
                return base::Result<contracts::CommandAcknowledgement>::failure(
                    {base::ErrorCode::kConflict, "mount.already_mounted"});
            }
        }
    }

    auto repository = impl_->control_plane->get_repository_connection(
        command.repository_connection_id, cancellation);
    if (!repository || !repository.value()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            !repository ? repository.error()
                        : base::Error{base::ErrorCode::kNotFound, "repository connection not found"});
    }
    auto chain =
        resolve_chain(*impl_->control_plane, *impl_->storage_factory, command.repository_connection_id,
                      command.recovery_point_id, cancellation);
    if (!chain) {
        return base::Result<contracts::CommandAcknowledgement>::failure(chain.error());
    }

    std::vector<std::string> absolute_paths;
    absolute_paths.reserve(chain.value().size());
    for (const auto& entry : chain.value()) {
        auto absolute =
            resolve_archive_absolute_path(repository.value()->locator, entry.archive_main_key);
        if (!absolute) {
            return base::Result<contracts::CommandAcknowledgement>::failure(absolute.error());
        }
        absolute_paths.push_back(std::move(absolute).value());
    }

    auto session_id = random_id("mount-", *impl_->random, cancellation);
    if (!session_id) {
        return base::Result<contracts::CommandAcknowledgement>::failure(session_id.error());
    }
    auto pipe_name = generate_pipe_name(*impl_->random, cancellation);
    if (!pipe_name) {
        return base::Result<contracts::CommandAcknowledgement>::failure(pipe_name.error());
    }

    adapters::windows_ipc::WindowsNamedPipeListenRequest listen;
    listen.pipe_name = pipe_name.value();
    listen.maximum_frame_bytes = 1024U * 1024U;
    listen.pipe_namespace = adapters::windows_ipc::WindowsNamedPipeNamespace::kWorker;
    listen.acl_profile = adapters::windows_ipc::WindowsNamedPipeAclProfile::kProcessDefault;
    auto listener = WindowsNamedPipeListener::create(listen);
    if (!listener) {
        return base::Result<contracts::CommandAcknowledgement>::failure(listener.error());
    }

    ports::ProcessLaunchRequest launch;
    launch.executable_path = impl_->config.mount_host_executable_path;
    launch.arguments = {"--mount-pipe", pipe_name.value()};
    auto launched = impl_->launcher->launch(launch);
    if (!launched) {
        return base::Result<contracts::CommandAcknowledgement>::failure(launched.error());
    }

    base::CancellationSource accept_cancel;
    auto channel = listener.value()->accept(accept_cancel.get_token());
    if (!channel) {
        (void)impl_->launcher->terminate(launched.value().pid);
        (void)impl_->launcher->wait(launched.value().pid, {});
        return base::Result<contracts::CommandAcknowledgement>::failure(channel.error());
    }

    const auto overlay_dir = impl_->config.overlay_root / session_id.value();
    std::error_code ec;
    std::filesystem::create_directories(overlay_dir, ec);
    if (ec) {
        (void)impl_->launcher->terminate(launched.value().pid);
        (void)impl_->launcher->wait(launched.value().pid, {});
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kIoFailure, "mount overlay directory could not be created"});
    }

    // Tip catalog entry decides the mount path: whole-disk (volume_set) or
    // read-only file namespace (file_set).
    auto request_json = build_mount_request_json(command, session_id.value(),
                                                 chain.value().back().content_kind,
                                                 path_to_utf8(overlay_dir), absolute_paths);
    if (!request_json) {
        (void)impl_->launcher->terminate(launched.value().pid);
        (void)impl_->launcher->wait(launched.value().pid, {});
        return base::Result<contracts::CommandAcknowledgement>::failure(request_json.error());
    }
    auto sent = channel.value()->send(request_json.value(), cancellation);
    if (!sent) {
        (void)impl_->launcher->terminate(launched.value().pid);
        (void)impl_->launcher->wait(launched.value().pid, {});
        return base::Result<contracts::CommandAcknowledgement>::failure(sent.error());
    }

    auto event_frame = channel.value()->receive(cancellation);
    if (!event_frame) {
        (void)impl_->launcher->terminate(launched.value().pid);
        (void)impl_->launcher->wait(launched.value().pid, {});
        return base::Result<contracts::CommandAcknowledgement>::failure(event_frame.error());
    }
    auto event = parse_host_event(event_frame.value());
    if (!event) {
        (void)impl_->launcher->terminate(launched.value().pid);
        (void)impl_->launcher->wait(launched.value().pid, {});
        return base::Result<contracts::CommandAcknowledgement>::failure(event.error());
    }
    if (event.value().value("kind", "") != "mounted") {
        (void)impl_->launcher->terminate(launched.value().pid);
        (void)impl_->launcher->wait(launched.value().pid, {});
        const auto code = event.value().value("message_code", "mount.host_failed");
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, code});
    }

    auto live = std::make_shared<LiveSession>();
    live->summary.session_id = session_id.value();
    live->summary.recovery_point_id = command.recovery_point_id;
    live->summary.state = contracts::MountSessionState::kMounted;
    live->summary.mount_point = event.value().value("mount_point", "");
    live->summary.source_disk_number = command.source_disk_number;
    live->summary.disk_size_bytes = event.value().value("disk_size_bytes", 0ULL);
    live->summary.started_utc_ms = utc_now_ms(*impl_->clock);
    live->summary.message_code = "mount.session_mounted";
    live->repository_connection_id = command.repository_connection_id;
    live->source_disk_number = command.source_disk_number;
    live->idempotency_key = std::string(idempotency_key);
    live->request_fingerprint = fingerprint;
    live->pipe_name = pipe_name.value();
    live->host_pid = launched.value().pid;
    live->listener = std::move(listener).value();
    live->channel = std::move(channel).value();

    const auto pid = live->host_pid;
    auto* launcher = impl_->launcher;
    live->waiter = std::jthread([live, launcher, pid](const std::stop_token) {
        (void)launcher->wait(pid, {});
        live->finished = true;
    });

    {
        std::lock_guard lock(impl_->mutex);
        impl_->session_by_idempotency[std::string(idempotency_key)] = session_id.value();
        impl_->sessions_by_id[session_id.value()] = live;
    }

    return base::Result<contracts::CommandAcknowledgement>::success(
        acknowledgement(std::string(idempotency_key), contracts::CommandDisposition::kAccepted,
                        session_id.value()));
}

base::Result<contracts::CommandAcknowledgement>
MountSupervisor::unmount(const contracts::ResourceRef& session,
                         const std::string_view idempotency_key,
                         const base::CancellationToken cancellation) {
    if (session.resource_id.empty()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kInvalidArgument, "mount session id is required"});
    }
    std::shared_ptr<LiveSession> live;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->sessions_by_id.find(session.resource_id);
        if (found == impl_->sessions_by_id.end()) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kNotFound, "mount session was not found"});
        }
        live = found->second;
        live->summary.state = contracts::MountSessionState::kUnmounting;
        live->summary.message_code = "mount.session_unmounting";
    }

    if (live->channel) {
        const Json command{{"schema_version", 1},
                           {"operation", "unmount"},
                           {"session_id", live->summary.session_id}};
        (void)live->channel->send(command.dump(), cancellation);
        auto reply = live->channel->receive(cancellation);
        (void)reply;
    }
    if (live->host_pid != 0) {
        (void)impl_->launcher->terminate(live->host_pid);
        (void)impl_->launcher->wait(live->host_pid, cancellation);
    }
    if (live->waiter.joinable()) {
        live->waiter.request_stop();
        live->waiter.join();
    }

    {
        std::lock_guard lock(impl_->mutex);
        impl_->session_by_idempotency.erase(live->idempotency_key);
        impl_->sessions_by_id.erase(session.resource_id);
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        acknowledgement(std::string(idempotency_key), contracts::CommandDisposition::kAccepted,
                        session.resource_id));
}

base::Result<contracts::MountSessionPage>
MountSupervisor::list(const contracts::MountSessionListRequest& request,
                      const base::CancellationToken cancellation) const {
    static_cast<void>(cancellation);
    if (auto valid = contracts::validate_mount_session_list_request(request); !valid) {
        return base::Result<contracts::MountSessionPage>::failure(valid.error());
    }
    std::vector<contracts::MountSessionSummary> items;
    {
        std::lock_guard lock(impl_->mutex);
        items.reserve(impl_->sessions_by_id.size());
        for (const auto& [id, session] : impl_->sessions_by_id) {
            static_cast<void>(id);
            auto summary = session->summary;
            if (session->finished.load() && summary.state == contracts::MountSessionState::kMounted) {
                summary.state = contracts::MountSessionState::kFailed;
                summary.message_code = "mount.host_exited";
            }
            if (request.state && summary.state != *request.state) {
                continue;
            }
            items.push_back(std::move(summary));
        }
    }
    std::sort(items.begin(), items.end(),
              [](const contracts::MountSessionSummary& left,
                 const contracts::MountSessionSummary& right) {
                  return left.started_utc_ms > right.started_utc_ms;
              });
    const auto limit = request.page.maximum_results == 0
                           ? contracts::kMaximumServicePageResults
                           : request.page.maximum_results;
    if (items.size() > limit) {
        items.resize(limit);
    }
    contracts::MountSessionPage page;
    page.items = std::move(items);
    return base::Result<contracts::MountSessionPage>::success(std::move(page));
}

void MountSupervisor::shutdown(const base::CancellationToken& cancellation) {
    std::vector<std::shared_ptr<LiveSession>> sessions;
    {
        std::lock_guard lock(impl_->mutex);
        sessions.reserve(impl_->sessions_by_id.size());
        for (auto& [id, session] : impl_->sessions_by_id) {
            static_cast<void>(id);
            sessions.push_back(session);
        }
        impl_->sessions_by_id.clear();
        impl_->session_by_idempotency.clear();
    }
    for (const auto& session : sessions) {
        if (session->channel) {
            const Json command{{"schema_version", 1},
                               {"operation", "unmount"},
                               {"session_id", session->summary.session_id}};
            (void)session->channel->send(command.dump(), cancellation);
        }
        if (session->host_pid != 0) {
            (void)impl_->launcher->terminate(session->host_pid);
            (void)impl_->launcher->wait(session->host_pid, cancellation);
        }
        if (session->waiter.joinable()) {
            session->waiter.request_stop();
            session->waiter.join();
        }
    }
}

} // namespace aegra::apps::service
