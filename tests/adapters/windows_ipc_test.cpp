#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"

#include "aegra/base/error.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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

class UniqueHandle final {
  public:
    explicit UniqueHandle(const HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (valid()) {
                CloseHandle(handle_);
            }
            handle_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

  private:
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, INVALID_HANDLE_VALUE); }

    HANDLE handle_{INVALID_HANDLE_VALUE};
};

std::string unique_pipe_name() {
    static std::atomic_uint32_t sequence{0};
    return "test-" + std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(sequence.fetch_add(1));
}

std::wstring pipe_path(const std::string_view logical_name) {
    std::wstring path = LR"(\\.\pipe\aegra-worker-)";
    for (const char value : logical_name) {
        path.push_back(static_cast<wchar_t>(value));
    }
    return path;
}

class TestPipeServer final {
  public:
    TestPipeServer() : logical_name_(unique_pipe_name()) {
        pipe_ = UniqueHandle(CreateNamedPipeW(pipe_path(logical_name_).c_str(), PIPE_ACCESS_DUPLEX,
                                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, 1, 4096, 4096, 0,
                                              nullptr));
    }

    [[nodiscard]] bool valid() const noexcept { return pipe_.valid(); }
    [[nodiscard]] const std::string& logical_name() const noexcept { return logical_name_; }
    [[nodiscard]] HANDLE handle() const noexcept { return pipe_.get(); }

    [[nodiscard]] bool accept() const {
        return ConnectNamedPipe(pipe_.get(), nullptr) != FALSE ||
               GetLastError() == ERROR_PIPE_CONNECTED;
    }

  private:
    std::string logical_name_;
    UniqueHandle pipe_;
};

std::array<std::byte, 4> encode_length(const std::uint32_t length) {
    return {
        static_cast<std::byte>(length & 0xFFU),
        static_cast<std::byte>((length >> 8U) & 0xFFU),
        static_cast<std::byte>((length >> 16U) & 0xFFU),
        static_cast<std::byte>((length >> 24U) & 0xFFU),
    };
}

bool write_bytes(const HANDLE pipe, const std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.subspan(offset);
        DWORD written = 0;
        const auto size = static_cast<DWORD>(remaining.size());
        if (!WriteFile(pipe, remaining.data(), size, &written, nullptr) || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool read_bytes(const HANDLE pipe, const std::span<std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.subspan(offset);
        DWORD read = 0;
        const auto size = static_cast<DWORD>(remaining.size());
        if (!ReadFile(pipe, remaining.data(), size, &read, nullptr) || read == 0) {
            return false;
        }
        offset += read;
    }
    return true;
}

bool write_frame(const HANDLE pipe, const std::string_view message) {
    const auto header = encode_length(static_cast<std::uint32_t>(message.size()));
    const auto body = std::as_bytes(std::span<const char>(message.data(), message.size()));
    return write_bytes(pipe, header) && write_bytes(pipe, body);
}

std::string read_frame(const HANDLE pipe) {
    std::array<std::byte, 4> header{};
    if (!read_bytes(pipe, header)) {
        return {};
    }
    const auto length = std::to_integer<std::uint32_t>(header[0]) |
                        (std::to_integer<std::uint32_t>(header[1]) << 8U) |
                        (std::to_integer<std::uint32_t>(header[2]) << 16U) |
                        (std::to_integer<std::uint32_t>(header[3]) << 24U);
    std::string result(length, '\0');
    return read_bytes(pipe, std::as_writable_bytes(std::span<char>(result))) ? result
                                                                             : std::string{};
}

bool test_connect_validation() {
    auto invalid = windows_ipc::WindowsNamedPipeChannel::connect({"../remote"}, {});
    bool passed =
        expect(!invalid && invalid.error().code == aegra::base::ErrorCode::kInvalidArgument,
               "invalid logical pipe name is rejected");
    aegra::base::CancellationSource cancellation;
    cancellation.request_stop();
    auto cancelled = windows_ipc::WindowsNamedPipeChannel::connect({unique_pipe_name(), 1'000},
                                                                   cancellation.get_token());
    passed &= expect(!cancelled && cancelled.error().code == aegra::base::ErrorCode::kCancelled,
                     "pre-cancelled pipe connection is rejected");
    return passed;
}

bool test_bidirectional_roundtrip() {
    TestPipeServer server;
    if (!expect(server.valid(), "test pipe server is created")) {
        return false;
    }
    auto channel = windows_ipc::WindowsNamedPipeChannel::connect({server.logical_name()}, {});
    if (!expect(channel.has_value() && server.accept(), "named pipe client connects locally")) {
        return false;
    }
    auto server_exchange = std::async(std::launch::async, [&server] {
        return write_frame(server.handle(), "parent-to-worker") &&
               read_frame(server.handle()) == "worker-to-parent";
    });
    auto receive = channel.value()->receive({});
    auto send = channel.value()->send("worker-to-parent", {});
    return expect(receive && receive.value() == "parent-to-worker", "server frame is received") &&
           expect(send.has_value(), "client frame is sent") &&
           expect(server_exchange.get(), "one reader and one writer can run concurrently");
}

bool test_invalid_frame_lengths() {
    TestPipeServer zero_server;
    auto zero_channel =
        windows_ipc::WindowsNamedPipeChannel::connect({zero_server.logical_name()}, {});
    bool passed = expect(zero_channel && zero_server.accept(), "zero frame test connects");
    if (!zero_channel) {
        return false;
    }
    const auto zero = encode_length(0);
    passed &= expect(write_bytes(zero_server.handle(), zero), "zero frame header is written");
    auto zero_result = zero_channel.value()->receive({});
    passed &=
        expect(!zero_result && zero_result.error().code == aegra::base::ErrorCode::kInvalidArgument,
               "zero-length frame is rejected");

    TestPipeServer large_server;
    auto large_channel = windows_ipc::WindowsNamedPipeChannel::connect(
        {large_server.logical_name(), 10'000, 16}, {});
    passed &= expect(large_channel && large_server.accept(), "large frame test connects");
    if (!large_channel) {
        return false;
    }
    const auto large = encode_length(17);
    passed &= expect(write_bytes(large_server.handle(), large), "large frame header is written");
    auto large_result = large_channel.value()->receive({});
    passed &= expect(!large_result &&
                         large_result.error().code == aegra::base::ErrorCode::kInvalidArgument,
                     "oversized frame is rejected");
    passed &= expect(!large_channel.value()->send("", {}), "empty outbound frame is rejected");
    passed &= expect(!large_channel.value()->send("12345678901234567", {}),
                     "oversized outbound frame is rejected");
    return passed;
}

bool test_receive_cancellation() {
    TestPipeServer server;
    auto channel = windows_ipc::WindowsNamedPipeChannel::connect({server.logical_name()}, {});
    if (!expect(channel && server.accept(), "cancellation test connects")) {
        return false;
    }
    aegra::base::CancellationSource cancellation;
    auto pending = std::async(std::launch::async,
                              [&] { return channel.value()->receive(cancellation.get_token()); });
    const auto state = pending.wait_for(std::chrono::milliseconds(50));
    cancellation.request_stop();
    auto result = pending.get();
    return expect(state == std::future_status::timeout, "receive remains pending before cancel") &&
           expect(!result && result.error().code == aegra::base::ErrorCode::kCancelled,
                  "pending receive is cancelled");
}

bool test_server_disconnect() {
    auto server = std::make_unique<TestPipeServer>();
    auto channel = windows_ipc::WindowsNamedPipeChannel::connect({server->logical_name()}, {});
    if (!expect(channel && server->accept(), "disconnect test connects")) {
        return false;
    }
    server.reset();
    auto result = channel.value()->receive({});
    return expect(!result && result.error().code == aegra::base::ErrorCode::kIoFailure,
                  "server disconnect maps to I/O failure");
}

bool test_service_listener_roundtrip() {
    const auto name = unique_pipe_name();
    auto listener = windows_ipc::WindowsNamedPipeListener::create({name, 4096});
    if (!expect(listener.has_value(), "service pipe listener is created")) {
        return false;
    }
    aegra::base::CancellationSource accept_cancellation;
    auto accepted = std::async(std::launch::async, [&] {
        return listener.value()->accept(accept_cancellation.get_token());
    });
    auto client = windows_ipc::WindowsNamedPipeChannel::connect(
        {name, 2'000, 4096, windows_ipc::WindowsNamedPipeNamespace::kService}, {});
    if (!client) {
        accept_cancellation.request_stop();
        (void)accepted.get();
        return expect(false, "service pipe client connects to listener namespace");
    }
    auto server = accepted.get();
    if (!expect(server.has_value(), "service listener accepts a local client")) {
        return false;
    }
    auto server_exchange = std::async(std::launch::async, [&] {
        auto received = server.value()->receive({});
        auto sent = server.value()->send("service-to-desktop", {});
        return received && received.value() == "desktop-to-service" && sent.has_value();
    });
    auto sent = client.value()->send("desktop-to-service", {});
    auto received = client.value()->receive({});
    return expect(sent.has_value(), "service client sends a framed request") &&
           expect(received && received.value() == "service-to-desktop",
                  "service client receives a framed response") &&
           expect(server_exchange.get(), "accepted channel completes a bidirectional exchange");
}

bool test_service_listener_cancellation() {
    auto invalid = windows_ipc::WindowsNamedPipeListener::create({"../remote", 4096});
    bool passed =
        expect(!invalid && invalid.error().code == aegra::base::ErrorCode::kInvalidArgument,
               "invalid service listener name is rejected");
    auto listener = windows_ipc::WindowsNamedPipeListener::create({unique_pipe_name(), 4096});
    if (!expect(listener.has_value(), "cancellation listener is created")) {
        return false;
    }
    aegra::base::CancellationSource cancellation;
    auto pending = std::async(std::launch::async,
                              [&] { return listener.value()->accept(cancellation.get_token()); });
    const auto state = pending.wait_for(std::chrono::milliseconds(50));
    cancellation.request_stop();
    auto result = pending.get();
    passed &= expect(state == std::future_status::timeout,
                     "listener accept remains pending before cancellation");
    passed &= expect(!result && result.error().code == aegra::base::ErrorCode::kCancelled,
                     "pending listener accept is cancelled");
    return passed;
}

int run_tests() {
    const bool passed = test_connect_validation() && test_bidirectional_roundtrip() &&
                        test_invalid_frame_lengths() && test_receive_cancellation() &&
                        test_server_disconnect() && test_service_listener_roundtrip() &&
                        test_service_listener_cancellation();
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
