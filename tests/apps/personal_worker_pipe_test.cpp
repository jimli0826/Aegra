#include "aegra/contracts/worker_session.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kInvalidJob = R"({"schema_version":1})";

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
    UniqueHandle(UniqueHandle&&) = delete;
    UniqueHandle& operator=(UniqueHandle&&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

std::wstring pipe_path(const std::wstring_view logical_name) {
    return LR"(\\.\pipe\aegra-worker-)" + std::wstring(logical_name);
}

std::array<std::byte, 4> encode_length(const std::uint32_t length) {
    return {
        static_cast<std::byte>(length & 0xFFU),
        static_cast<std::byte>((length >> 8U) & 0xFFU),
        static_cast<std::byte>((length >> 16U) & 0xFFU),
        static_cast<std::byte>((length >> 24U) & 0xFFU),
    };
}

bool write_all(const HANDLE pipe, const std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.subspan(offset);
        DWORD written = 0;
        if (!WriteFile(pipe, remaining.data(), static_cast<DWORD>(remaining.size()), &written,
                       nullptr) ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool read_all(const HANDLE pipe, const std::span<std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.subspan(offset);
        DWORD read = 0;
        if (!ReadFile(pipe, remaining.data(), static_cast<DWORD>(remaining.size()), &read,
                      nullptr) ||
            read == 0) {
            return false;
        }
        offset += read;
    }
    return true;
}

bool write_frame(const HANDLE pipe, const std::string_view message) {
    const auto header = encode_length(static_cast<std::uint32_t>(message.size()));
    return write_all(pipe, header) &&
           write_all(pipe, std::as_bytes(std::span<const char>(message.data(), message.size())));
}

std::string read_frame(const HANDLE pipe) {
    std::array<std::byte, 4> header{};
    if (!read_all(pipe, header)) {
        return {};
    }
    const auto length = std::to_integer<std::uint32_t>(header[0]) |
                        (std::to_integer<std::uint32_t>(header[1]) << 8U) |
                        (std::to_integer<std::uint32_t>(header[2]) << 16U) |
                        (std::to_integer<std::uint32_t>(header[3]) << 24U);
    std::string result(length, '\0');
    return read_all(pipe, std::as_writable_bytes(std::span<char>(result))) ? result : std::string{};
}

std::wstring quote_argument(const std::wstring_view value) {
    return L"\"" + std::wstring(value) + L"\"";
}

bool test_pipe_rejection(const std::wstring_view worker_path) {
    const auto pipe_name = L"process-test-" + std::to_wstring(GetCurrentProcessId());
    UniqueHandle pipe(CreateNamedPipeW(pipe_path(pipe_name).c_str(), PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, 1, 4096, 4096, 0,
                                       nullptr));
    if (!expect(pipe.valid(), "parent named pipe is created")) {
        return false;
    }

    const std::wstring application_path(worker_path);
    auto command_line = quote_argument(application_path) + L" --pipe " + quote_argument(pipe_name);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!expect(CreateProcessW(application_path.c_str(), command_line.data(), nullptr, nullptr,
                               FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                               &process) != FALSE,
                "worker process starts in pipe mode")) {
        return false;
    }
    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);
    const bool connected =
        ConnectNamedPipe(pipe.get(), nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
    bool passed = expect(connected, "worker connects to parent pipe");
    passed &= expect(write_frame(pipe.get(), kInvalidJob), "parent writes invalid job frame");
    const auto frame = read_frame(pipe.get());
    passed &= expect(!frame.empty(), "worker returns one result frame");
    passed &= expect(WaitForSingleObject(process_handle.get(), 10'000) == WAIT_OBJECT_0,
                     "worker exits after final result");
    DWORD exit_code = 0;
    passed &=
        expect(GetExitCodeProcess(process_handle.get(), &exit_code) != FALSE && exit_code == 20,
               "invalid job exits with request-rejected code");

    passed &=
        expect(frame.find(R"("kind":2)") != std::string::npos, "pipe response is a result event");
    passed &= expect(frame.find(R"("message_code":"worker.request_rejected")") != std::string::npos,
                     "result event contains a request rejection");
    return passed;
}

int run_main(const std::span<const wchar_t* const> arguments) {
    if (arguments.size() != 2) {
        std::fputs("[FAIL] worker path is required\n", stderr);
        return EXIT_FAILURE;
    }
    return test_pipe_rejection(arguments[1]) ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int wmain(const int argument_count, const wchar_t* const* arguments) noexcept {
    try {
        return run_main({arguments, static_cast<std::size_t>(argument_count)});
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
