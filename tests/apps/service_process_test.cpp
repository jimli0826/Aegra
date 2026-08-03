#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/apps/service/service_protocol.h"
#include "aegra/contracts/service.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
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

std::wstring widen_ascii(const std::string& value) { return {value.begin(), value.end()}; }

std::string unique_pipe_name() {
    static std::atomic_uint32_t sequence{0};
    return "process-test-" + std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(sequence.fetch_add(1));
}

class ChildProcess final {
  public:
    ~ChildProcess() {
        if (process_ != nullptr) {
            if (!exited_) {
                TerminateProcess(process_, 99);
                (void)WaitForSingleObject(process_, 5'000);
            }
            CloseHandle(process_);
        }
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept
        : process_(std::exchange(other.process_, nullptr)),
          exited_(std::exchange(other.exited_, true)) {}
    ChildProcess& operator=(ChildProcess&&) = delete;

    [[nodiscard]] static ChildProcess start(const std::wstring& executable,
                                            const std::string& pipe_name) {
        ChildProcess child;
        std::wstring command = L"\"" + executable + L"\" --once --pipe " + widen_ascii(pipe_name);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            CloseHandle(process.hThread);
            child.process_ = process.hProcess;
        }
        return child;
    }

    [[nodiscard]] bool valid() const noexcept { return process_ != nullptr; }

    [[nodiscard]] bool wait_for_success() {
        if (WaitForSingleObject(process_, 5'000) != WAIT_OBJECT_0) {
            return false;
        }
        exited_ = true;
        DWORD exit_code = 0;
        return GetExitCodeProcess(process_, &exit_code) && exit_code == 0;
    }

  private:
    ChildProcess() = default;

    HANDLE process_{nullptr};
    bool exited_{false};
};

bool test_once_process(const std::wstring& service_executable) {
    const auto pipe_name = unique_pipe_name();
    auto process = ChildProcess::start(service_executable, pipe_name);
    if (!expect(process.valid(), "service child process starts")) {
        return false;
    }
    auto channel = windows_ipc::WindowsNamedPipeChannel::connect(
        {pipe_name, 5'000, static_cast<std::uint32_t>(app::kMaximumServiceFrameBytes),
         windows_ipc::WindowsNamedPipeNamespace::kService},
        {});
    if (!expect(channel.has_value(), "test client connects to --once service")) {
        return false;
    }
    contracts::ServiceRequest request;
    request.request_id = "process-request-1";
    auto encoded = app::encode_service_request(request);
    if (!expect(encoded.has_value(), "process test request encodes")) {
        return false;
    }
    auto sent = channel.value()->send(encoded.value(), {});
    auto received = channel.value()->receive({});
    auto response = received ? app::decode_service_response(received.value())
                             : app::decode_service_response({});
    bool passed =
        expect(sent.has_value(), "request is sent to service process") &&
        expect(response && response.value().request_id == "process-request-1" &&
                   response.value().service && response.value().service->service_version == "0.1.0",
               "service process returns correlated Ready info");
    channel.value().reset();
    passed &= expect(process.wait_for_success(), "--once service exits successfully");
    return passed;
}

} // namespace

int wmain(const int argument_count, wchar_t* arguments[]) noexcept {
    try {
        if (argument_count != 2) {
            std::fputs("[FAIL] expected aegra_service executable path\n", stderr);
            return EXIT_FAILURE;
        }
        return test_once_process(arguments[1]) ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
