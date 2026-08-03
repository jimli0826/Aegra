#include "aegra/apps/service/service_security_host.h"
#include "aegra/apps/service/service_protocol.h"
#include "aegra/apps/service/windows_service_control.h"
#include "aegra/apps/service/windows_service_scm_host.h"

#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/base/error.h"
#include "aegra/contracts/service.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

namespace app = aegra::apps::service;
namespace contracts = aegra::contracts;
namespace windows_ipc = aegra::adapters::windows_ipc;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

std::string unique_pipe_name() {
    static std::atomic_uint32_t sequence{0};
    return "host-" + std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(sequence.fetch_add(1));
}

app::ServiceRuntimeInfo runtime_info() {
    return {.service_version = "0.1.0", .capabilities = {"service.info"}};
}

class FailStopPendingReporter final : public app::IWindowsServiceStatusReporter {
  public:
    [[nodiscard]] aegra::base::Result<void>
    report(const app::WindowsServiceStatus& status) override {
        if (status.state == app::WindowsServiceState::kStopPending) {
            return aegra::base::Result<void>::failure(
                aegra::base::Error{aegra::base::ErrorCode::kIoFailure,
                                   "stop-pending report failed"});
        }
        return aegra::base::Result<void>::success();
    }
};

std::string service_info_request_json() {
    contracts::ServiceRequest request;
    request.request_id = "req-security-1";
    auto encoded = app::encode_service_request(request);
    return encoded ? encoded.value() : std::string{};
}

bool test_authorized_once_success() {
    const auto name = unique_pipe_name();
    windows_ipc::WindowsNamedPipeListenRequest listen;
    listen.pipe_name = name;
    listen.maximum_frame_bytes = static_cast<std::uint32_t>(app::kMaximumServiceFrameBytes);
    listen.acl_profile = windows_ipc::WindowsNamedPipeAclProfile::kProcessDefault;
    auto listener = windows_ipc::WindowsNamedPipeListener::create(listen);
    if (!expect(listener.has_value(), "security host listener is created")) {
        return false;
    }
    app::ServiceSecurityHostOptions options;
    options.once = true;
    options.maximum_requests_per_session = 1;
    auto host = std::async(std::launch::async, [&] {
        return app::run_authorized_service_host(*listener.value(), runtime_info(), options, {});
    });
    auto client = windows_ipc::WindowsNamedPipeChannel::connect(
        {name, 3'000, static_cast<std::uint32_t>(app::kMaximumServiceFrameBytes),
         windows_ipc::WindowsNamedPipeNamespace::kService},
        {});
    if (!client) {
        // Ensure the background accept is cancelled if connect fails.
        // Host has no external cancel here; connect failure should be rare with process-default ACL.
        (void)host.wait_for(std::chrono::milliseconds(100));
        return expect(false, "desktop-equivalent client connects to authorized host");
    }
    const auto request = service_info_request_json();
    if (!expect(!request.empty(), "service info request encodes")) {
        return false;
    }
    auto sent = client.value()->send(request, {});
    auto received = client.value()->receive({});
    auto host_result = host.get();
    bool passed = expect(sent.has_value(), "client sends service.info") &&
                  expect(received.has_value(), "client receives service.info response") &&
                  expect(host_result.has_value(), "authorized once host succeeds");
    if (received) {
        auto response = app::decode_service_response(received.value());
        passed &= expect(response && response.value().message_code == "service.ready",
                         "authorized session returns service.ready");
    }
    return passed;
}

bool test_stop_cancels_pending_accept() {
    auto listener = windows_ipc::WindowsNamedPipeListener::create(
        {unique_pipe_name(), static_cast<std::uint32_t>(app::kMaximumServiceFrameBytes),
         windows_ipc::WindowsNamedPipeAclProfile::kProcessDefault});
    if (!expect(listener.has_value(), "stop host listener is created")) {
        return false;
    }
    app::ServiceSecurityHostOptions options;
    options.once = true;
    options.stop_deadline = std::chrono::milliseconds{1'000};
    aegra::base::CancellationSource cancellation;
    auto host = std::async(std::launch::async, [&] {
        return app::run_authorized_service_host(*listener.value(), runtime_info(), options,
                                                cancellation.get_token());
    });
    const auto pending = host.wait_for(std::chrono::milliseconds(50));
    cancellation.request_stop();
    auto result = host.get();
    return expect(pending == std::future_status::timeout, "host waits on accept before stop") &&
           expect(!result && result.error().code == aegra::base::ErrorCode::kCancelled,
                  "stop cancels pending accept within deadline");
}

bool test_unauthorized_identity_is_rejected() {
    windows_ipc::WindowsNamedPipePeerIdentity peer;
    peer.user_sid = "S-1-5-18";
    peer.session_id = 0;
    peer.process_id = 4;
    peer.is_local = true;
    peer.is_interactive = false;
    peer.is_administrator = false;
    auto denied =
        windows_ipc::authorize_service_caller(peer, windows_ipc::WindowsServiceCallerAuthorization{});
    return expect(!denied && denied.error().code == aegra::base::ErrorCode::kUnauthorized,
                  "session-0 service account identity is unauthorized");
}

bool test_install_recovery_restart_and_rollback() {
    app::FakeWindowsServiceControlManager manager;
    app::WindowsServiceInstallRequest request;
    request.service_name = "AegraServiceTest";
    request.display_name = "Aegra Service Test";
    request.binary_path = R"(C:\Program Files\Aegra\aegra_service.exe)";
    request.recovery_delay_ms = 3'000;
    bool passed = expect(app::install_windows_service(manager, request).has_value(),
                         "install succeeds with recovery");
    auto identity = manager.query_service(request.service_name);
    passed &= expect(identity && identity.value().recovery_enabled, "recovery is configured") &&
              expect(identity && identity.value().recovery_delay_ms == 3'000,
                     "recovery delay is stored");

    passed &= expect(manager.start_service(request.service_name).has_value(), "service starts");
    passed &= expect(app::restart_windows_service(manager, request.service_name).has_value(),
                     "restart stops then starts");
    auto after_restart = manager.query_service(request.service_name);
    passed &= expect(after_restart.has_value(), "service remains registered after restart");

    passed &= expect(app::uninstall_windows_service(manager, request.service_name).has_value(),
                     "uninstall stops and deletes");
    auto missing = manager.query_service(request.service_name);
    passed &= expect(!missing && missing.error().code == aegra::base::ErrorCode::kNotFound,
                     "uninstalled service is gone");

    app::FakeWindowsServiceControlManager rollback_manager;
    rollback_manager.set_fail_recovery(true);
    auto rolled = app::install_windows_service(rollback_manager, request);
    passed &= expect(!rolled && rolled.error().code == aegra::base::ErrorCode::kIoFailure,
                     "recovery failure fails install");
    auto after_rollback = rollback_manager.query_service(request.service_name);
    passed &=
        expect(!after_rollback && after_rollback.error().code == aegra::base::ErrorCode::kNotFound,
               "failed install rolls back service registration");
    const auto& operations = rollback_manager.operations();
    passed &= expect(operations.size() >= 3 && operations[0] == "create" &&
                         operations[1] == "configure_recovery" && operations[2] == "delete",
                     "rollback deletes after failed recovery configuration");
    return passed;
}

bool test_scm_host_start_stop_status() {
    app::WindowsServiceScmHost host(std::chrono::milliseconds{1'000});
    app::RecordingServiceStatusReporter reporter;
    auto runner = std::async(std::launch::async, [&] {
        return host.run(reporter, [](const aegra::base::CancellationToken& cancellation) {
            while (!cancellation.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return aegra::base::Result<void>::failure(
                aegra::base::Error{aegra::base::ErrorCode::kCancelled, "service stop requested"});
        });
    });
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (host.status().state == app::WindowsServiceState::kRunning) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool passed = expect(host.status().state == app::WindowsServiceState::kRunning,
                         "SCM host reports running") &&
                  expect(host.status().accepts_stop, "running service accepts stop");
    host.request_stop();
    auto result = runner.get();
    passed &= expect(result.has_value(), "cooperative cancel is a clean service stop") &&
              expect(host.status().state == app::WindowsServiceState::kStopped,
                     "SCM host reports stopped") &&
              expect(reporter.history().size() >= 3, "status transitions are reported");
    if (reporter.history().size() >= 3) {
        passed &= expect(reporter.history()[0].state == app::WindowsServiceState::kStartPending,
                         "first status is start pending") &&
                  expect(reporter.history()[1].state == app::WindowsServiceState::kRunning,
                         "second status is running") &&
                  expect(reporter.history().back().state == app::WindowsServiceState::kStopped,
                         "final status is stopped");
    }
    return passed;
}

bool test_scm_host_stop_deadline_noncooperative_worker() {
    app::WindowsServiceScmHost host(std::chrono::milliseconds{200});
    app::RecordingServiceStatusReporter reporter;
    auto runner = std::async(std::launch::async, [&] {
        return host.run(reporter, [](const aegra::base::CancellationToken&) {
            // Ignore cancellation to prove the host still returns after stop_deadline.
            std::this_thread::sleep_for(std::chrono::seconds(30));
            return aegra::base::Result<void>::success();
        });
    });
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (host.status().state == app::WindowsServiceState::kRunning) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!expect(host.status().state == app::WindowsServiceState::kRunning,
                "non-cooperative worker reaches running")) {
        host.request_stop();
        (void)runner.wait_for(std::chrono::milliseconds(500));
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    host.request_stop();
    auto result = runner.get();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    return expect(!result && result.error().code == aegra::base::ErrorCode::kInternal,
                  "non-cooperative worker exceeds stop deadline") &&
           expect(elapsed < std::chrono::seconds(5),
                  "stop deadline returns without waiting for the abandoned worker") &&
           expect(host.status().state == app::WindowsServiceState::kStopped,
                  "deadline path still reports stopped");
}

bool test_scm_host_converts_worker_exception() {
    app::WindowsServiceScmHost host(std::chrono::milliseconds{1'000});
    app::RecordingServiceStatusReporter reporter;
    auto result = host.run(
        reporter,
        [](const aegra::base::CancellationToken&) -> aegra::base::Result<void> {
            throw std::runtime_error("worker failure");
        });
    return expect(!result && result.error().code == aegra::base::ErrorCode::kInternal,
                  "worker exception is converted at the thread boundary") &&
           expect(host.status().state == app::WindowsServiceState::kStopped,
                  "worker exception still reports stopped");
}

bool test_scm_host_report_failure_preserves_stop_deadline() {
    app::WindowsServiceScmHost host(std::chrono::milliseconds{100});
    FailStopPendingReporter reporter;
    auto runner = std::async(std::launch::async, [&] {
        return host.run(reporter, [](const aegra::base::CancellationToken&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            return aegra::base::Result<void>::success();
        });
    });
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (host.status().state == app::WindowsServiceState::kRunning) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto started = std::chrono::steady_clock::now();
    host.request_stop();
    auto result = runner.get();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    return expect(!result && result.error().code == aegra::base::ErrorCode::kInternal,
                  "stop deadline wins when stop-pending status reporting fails") &&
           expect(elapsed < std::chrono::milliseconds(400),
                  "status reporting failure does not cause an unbounded join") &&
           expect(host.status().state == app::WindowsServiceState::kStopped,
                  "report failure deadline path still records stopped");
}

int run_tests() {
    const bool passed =
        test_authorized_once_success() && test_stop_cancels_pending_accept() &&
        test_unauthorized_identity_is_rejected() && test_install_recovery_restart_and_rollback() &&
        test_scm_host_start_stop_status() && test_scm_host_converts_worker_exception() &&
        test_scm_host_report_failure_preserves_stop_deadline() &&
        test_scm_host_stop_deadline_noncooperative_worker();
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main() noexcept {
    try {
        return run_tests();
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
