#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/adapters/windows_ipc/windows_named_pipe_security.h"

#include "aegra/base/error.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <utility>

namespace {

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
    return "sec-" + std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(sequence.fetch_add(1));
}

windows_ipc::WindowsNamedPipePeerIdentity sample_peer(const std::string& sid,
                                                      const std::uint32_t session_id,
                                                      const bool interactive,
                                                      const bool administrator) {
    windows_ipc::WindowsNamedPipePeerIdentity peer;
    peer.user_sid = sid;
    peer.session_id = session_id;
    peer.process_id = 1000U + session_id;
    peer.is_local = true;
    peer.is_interactive = interactive;
    peer.is_administrator = administrator;
    return peer;
}

bool test_authorize_success_and_multi_session() {
    windows_ipc::WindowsServiceCallerAuthorization policy;
    const auto session_one = sample_peer("S-1-5-21-1-2-3-1001", 1, true, false);
    const auto session_two = sample_peer("S-1-5-21-1-2-3-1002", 2, true, false);
    return expect(windows_ipc::authorize_service_caller(session_one, policy).has_value(),
                  "interactive session one is authorized") &&
           expect(windows_ipc::authorize_service_caller(session_two, policy).has_value(),
                  "interactive session two is authorized for multi-session personal use");
}

bool test_authorize_rejects_incomplete_and_service_accounts() {
    windows_ipc::WindowsServiceCallerAuthorization policy;
    auto remote = sample_peer("S-1-5-21-1-2-3-1003", 1, true, false);
    remote.is_local = false;
    auto denied_remote = windows_ipc::authorize_service_caller(remote, policy);
    bool passed = expect(!denied_remote &&
                             denied_remote.error().code == aegra::base::ErrorCode::kUnauthorized,
                         "non-local peer is rejected");

    auto service_account = sample_peer("S-1-5-18", 0, false, false);
    auto denied_service = windows_ipc::authorize_service_caller(service_account, policy);
    passed &= expect(!denied_service &&
                         denied_service.error().code == aegra::base::ErrorCode::kUnauthorized,
                     "non-interactive service account peer is rejected");
    return passed;
}

bool test_process_default_accept_and_peer_identity() {
    // Interactive/dev mode uses process-default DACL with remote clients rejected.
    // Process-default ACL is used only for private same-user process communication.
    const auto name = unique_pipe_name();
    windows_ipc::WindowsNamedPipeListenRequest request;
    request.pipe_name = name;
    request.maximum_frame_bytes = 4096;
    request.acl_profile = windows_ipc::WindowsNamedPipeAclProfile::kProcessDefault;
    auto listener = windows_ipc::WindowsNamedPipeListener::create(request);
    if (!expect(listener.has_value(), "process-default listener is created")) {
        return false;
    }
    aegra::base::CancellationSource accept_cancellation;
    auto accepted = std::async(std::launch::async, [&] {
        return listener.value()->accept_authorized(windows_ipc::WindowsServiceCallerAuthorization{},
                                                   accept_cancellation.get_token());
    });
    auto client = windows_ipc::WindowsNamedPipeChannel::connect(
        {name, 2'000, 4096, windows_ipc::WindowsNamedPipeNamespace::kService}, {});
    if (!client) {
        accept_cancellation.request_stop();
        (void)accepted.get();
        return expect(false, "local client connects through process-default service pipe");
    }
    auto server = accepted.get();
    if (!expect(server.has_value(), "authorized local interactive client is accepted")) {
        return false;
    }
    bool passed = expect(!server.value().peer.user_sid.empty(), "peer SID is captured") &&
                  expect(server.value().peer.process_id != 0, "peer process id is captured") &&
                  expect(server.value().peer.is_local, "peer is marked local");
    auto exchange = std::async(std::launch::async, [&] {
        auto received = server.value().channel->receive({});
        auto sent = server.value().channel->send("acl-ok", {});
        return received && received.value() == "client-hello" && sent.has_value();
    });
    auto sent = client.value()->send("client-hello", {});
    auto received = client.value()->receive({});
    passed &= expect(sent.has_value(), "client sends after authorized accept") &&
              expect(received && received.value() == "acl-ok",
                     "client receives after authorized accept") &&
              expect(exchange.get(), "authorized session exchanges frames");
    return passed;
}

bool test_local_everyone_control_profile_accepts_client() {
    const auto name = unique_pipe_name();
    auto listener = windows_ipc::WindowsNamedPipeListener::create(
        {name, 4096, windows_ipc::WindowsNamedPipeAclProfile::kLocalEveryoneControl});
    if (!expect(listener.has_value(), "local everyone control listener is created")) {
        return false;
    }
    aegra::base::CancellationSource cancellation;
    auto accepted = std::async(std::launch::async,
                               [&] { return listener.value()->accept(cancellation.get_token()); });
    auto client = windows_ipc::WindowsNamedPipeChannel::connect(
        {name, 2'000, 4096, windows_ipc::WindowsNamedPipeNamespace::kService}, {});
    if (!client) {
        cancellation.request_stop();
        (void)accepted.get();
        return expect(false, "local client connects through everyone control pipe");
    }
    auto server = accepted.get();
    if (!expect(server.has_value(), "everyone control listener accepts local client")) {
        return false;
    }
    auto exchange = std::async(std::launch::async, [&] {
        auto received = server.value()->receive({});
        auto sent = server.value()->send("acl-ok", {});
        return received && received.value() == "client-hello" && sent.has_value();
    });
    auto sent = client.value()->send("client-hello", {});
    auto received = client.value()->receive({});
    return expect(sent.has_value(), "client sends through everyone control pipe") &&
           expect(received && received.value() == "acl-ok",
                  "client receives through everyone control pipe") &&
           expect(exchange.get(), "everyone control session exchanges frames");
}

bool test_accept_cancellation_with_process_default() {
    auto listener = windows_ipc::WindowsNamedPipeListener::create(
        {unique_pipe_name(), 4096, windows_ipc::WindowsNamedPipeAclProfile::kProcessDefault});
    if (!expect(listener.has_value(), "cancellation listener is created")) {
        return false;
    }
    aegra::base::CancellationSource cancellation;
    auto pending = std::async(std::launch::async, [&] {
        return listener.value()->accept_authorized({}, cancellation.get_token());
    });
    const auto state = pending.wait_for(std::chrono::milliseconds(50));
    cancellation.request_stop();
    auto result = pending.get();
    return expect(state == std::future_status::timeout, "accept stays pending") &&
           expect(!result && result.error().code == aegra::base::ErrorCode::kCancelled,
                  "pending accept is cancelled within stop path");
}

int run_tests() {
    const bool passed = test_authorize_success_and_multi_session() &&
                        test_authorize_rejects_incomplete_and_service_accounts() &&
                        test_process_default_accept_and_peer_identity() &&
                        test_local_everyone_control_profile_accepts_client() &&
                        test_accept_cancellation_with_process_default();
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
