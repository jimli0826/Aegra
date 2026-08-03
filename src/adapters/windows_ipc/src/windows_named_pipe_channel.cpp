#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace aegra::adapters::windows_ipc {
namespace {

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
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, INVALID_HANDLE_VALUE); }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

bool valid_pipe_name(const std::string_view name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    for (const unsigned char value : name) {
        const bool alpha_numeric = (value >= 'a' && value <= 'z') ||
                                   (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
        if (!alpha_numeric && value != '-' && value != '_' && value != '.') {
            return false;
        }
    }
    return true;
}

bool valid_pipe_namespace(const WindowsNamedPipeNamespace value) noexcept {
    return value == WindowsNamedPipeNamespace::kWorker ||
           value == WindowsNamedPipeNamespace::kService;
}

std::wstring pipe_path(const WindowsNamedPipeNamespace pipe_namespace,
                       const std::string_view name) {
    std::wstring result = pipe_namespace == WindowsNamedPipeNamespace::kWorker
                              ? LR"(\\.\pipe\aegra-worker-)"
                              : LR"(\\.\pipe\aegra-service-)";
    result.reserve(result.size() + name.size());
    for (const char value : name) {
        result.push_back(static_cast<wchar_t>(value));
    }
    return result;
}

base::Error io_error(const DWORD error, const char* message) {
    if (error == ERROR_OPERATION_ABORTED) {
        return base::Error{base::ErrorCode::kCancelled, "named pipe I/O cancelled"};
    }
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY) {
        return base::Error{base::ErrorCode::kNotFound, "named pipe is unavailable"};
    }
    return base::Error{base::ErrorCode::kIoFailure, message};
}

struct CancelPendingIo final {
    HANDLE pipe;
    OVERLAPPED* overlapped;

    void operator()() const noexcept { CancelIoEx(pipe, overlapped); }
};

template <typename Byte>
base::Result<std::size_t> transfer_some(HANDLE pipe, const std::span<Byte> buffer,
                                        const base::CancellationToken& cancellation) {
    static_assert(std::is_same_v<std::remove_const_t<Byte>, std::byte>);
    if (cancellation.stop_requested()) {
        return base::Result<std::size_t>::failure(
            base::Error{base::ErrorCode::kCancelled, "named pipe I/O cancelled"});
    }
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return base::Result<std::size_t>::failure(
            base::Error{base::ErrorCode::kInternal, "named pipe event creation failed"});
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD transferred = 0;
    const auto length = static_cast<DWORD>(buffer.size());
    BOOL started = FALSE;
    if constexpr (std::is_const_v<Byte>) {
        started = WriteFile(pipe, buffer.data(), length, &transferred, &overlapped);
    } else {
        started = ReadFile(pipe, buffer.data(), length, &transferred, &overlapped);
    }
    if (started) {
        return base::Result<std::size_t>::success(transferred);
    }
    const auto start_error = GetLastError();
    if (start_error != ERROR_IO_PENDING) {
        return base::Result<std::size_t>::failure(io_error(start_error, "named pipe I/O failed"));
    }
    std::stop_callback cancel(cancellation, CancelPendingIo{pipe, &overlapped});
    (void)WaitForSingleObject(event.get(), INFINITE);
    if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
        return base::Result<std::size_t>::failure(
            io_error(GetLastError(), "named pipe overlapped I/O failed"));
    }
    return base::Result<std::size_t>::success(transferred);
}

template <typename Byte>
base::Result<void> transfer_all(HANDLE pipe, const std::span<Byte> buffer,
                                const base::CancellationToken& cancellation) {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        auto result = transfer_some(pipe, buffer.subspan(offset), cancellation);
        if (!result) {
            return base::Result<void>::failure(result.error());
        }
        if (result.value() == 0) {
            return base::Result<void>::failure(
                base::Error{base::ErrorCode::kIoFailure, "named pipe closed during frame"});
        }
        offset += result.value();
    }
    return base::Result<void>::success();
}

std::array<std::byte, 4> encode_length(const std::uint32_t length) noexcept {
    return {
        static_cast<std::byte>(length & 0xFFU),
        static_cast<std::byte>((length >> 8U) & 0xFFU),
        static_cast<std::byte>((length >> 16U) & 0xFFU),
        static_cast<std::byte>((length >> 24U) & 0xFFU),
    };
}

std::uint32_t decode_length(const std::array<std::byte, 4>& value) noexcept {
    return std::to_integer<std::uint32_t>(value[0]) |
           (std::to_integer<std::uint32_t>(value[1]) << 8U) |
           (std::to_integer<std::uint32_t>(value[2]) << 16U) |
           (std::to_integer<std::uint32_t>(value[3]) << 24U);
}

base::Result<UniqueHandle> connect_pipe(const std::wstring& path, const std::uint32_t timeout_ms,
                                        const base::CancellationToken& cancellation) {
    constexpr auto kPollInterval = std::chrono::milliseconds(50);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!cancellation.stop_requested()) {
        UniqueHandle pipe(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (pipe.valid()) {
            return base::Result<UniqueHandle>::success(std::move(pipe));
        }
        const auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
            return base::Result<UniqueHandle>::failure(
                io_error(error, "named pipe connect failed"));
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto interval = (std::min)(remaining, kPollInterval);
        const auto interval_ms = static_cast<DWORD>((std::max)(interval.count(), 1LL));
        if (error == ERROR_PIPE_BUSY) {
            (void)WaitNamedPipeW(path.c_str(), interval_ms);
        } else {
            Sleep(interval_ms);
        }
    }
    const auto code =
        cancellation.stop_requested() ? base::ErrorCode::kCancelled : base::ErrorCode::kNotFound;
    return base::Result<UniqueHandle>::failure(base::Error{code, "named pipe connect failed"});
}

} // namespace

struct WindowsNamedPipeChannel::Impl final {
    UniqueHandle pipe;
    std::uint32_t maximum_frame_bytes{0};
};

WindowsNamedPipeChannel::WindowsNamedPipeChannel(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

WindowsNamedPipeChannel::~WindowsNamedPipeChannel() = default;

std::unique_ptr<WindowsNamedPipeChannel>
WindowsNamedPipeChannel::adopt_connected(const std::uintptr_t native_handle,
                                         const std::uint32_t maximum_frame_bytes) {
    UniqueHandle owned(reinterpret_cast<HANDLE>(native_handle));
    auto impl = std::make_unique<Impl>();
    impl->pipe = std::move(owned);
    impl->maximum_frame_bytes = maximum_frame_bytes;
    return std::unique_ptr<WindowsNamedPipeChannel>(new WindowsNamedPipeChannel(std::move(impl)));
}

base::Result<std::unique_ptr<WindowsNamedPipeChannel>>
WindowsNamedPipeChannel::connect(const WindowsNamedPipeConnectRequest& request,
                                 const base::CancellationToken& cancellation) {
    if (!valid_pipe_name(request.pipe_name) || request.connect_timeout_ms == 0 ||
        request.maximum_frame_bytes == 0 || !valid_pipe_namespace(request.pipe_namespace)) {
        return base::Result<std::unique_ptr<WindowsNamedPipeChannel>>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "named pipe request is invalid"});
    }
    auto pipe = connect_pipe(pipe_path(request.pipe_namespace, request.pipe_name),
                             request.connect_timeout_ms, cancellation);
    if (!pipe) {
        return base::Result<std::unique_ptr<WindowsNamedPipeChannel>>::failure(pipe.error());
    }
    const auto native_handle = reinterpret_cast<std::uintptr_t>(pipe.value().release());
    return base::Result<std::unique_ptr<WindowsNamedPipeChannel>>::success(
        adopt_connected(native_handle, request.maximum_frame_bytes));
}

base::Result<std::string>
WindowsNamedPipeChannel::receive(const base::CancellationToken& cancellation) {
    std::array<std::byte, 4> header{};
    auto header_read = transfer_all(impl_->pipe.get(), std::span<std::byte>(header), cancellation);
    if (!header_read) {
        return base::Result<std::string>::failure(header_read.error());
    }
    const auto length = decode_length(header);
    if (length == 0 || length > impl_->maximum_frame_bytes) {
        return base::Result<std::string>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "named pipe frame length is invalid"});
    }
    std::string message(length, '\0');
    auto bytes = std::as_writable_bytes(std::span<char>(message));
    auto body_read = transfer_all(impl_->pipe.get(), bytes, cancellation);
    if (!body_read) {
        return base::Result<std::string>::failure(body_read.error());
    }
    return base::Result<std::string>::success(std::move(message));
}

base::Result<void> WindowsNamedPipeChannel::send(const std::string_view message,
                                                 const base::CancellationToken& cancellation) {
    if (message.empty() || message.size() > impl_->maximum_frame_bytes ||
        message.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "named pipe frame is invalid"});
    }
    auto header = encode_length(static_cast<std::uint32_t>(message.size()));
    auto header_written =
        transfer_all(impl_->pipe.get(), std::span<const std::byte>(header), cancellation);
    if (!header_written) {
        return header_written;
    }
    const auto body = std::as_bytes(std::span<const char>(message.data(), message.size()));
    return transfer_all(impl_->pipe.get(), body, cancellation);
}

} // namespace aegra::adapters::windows_ipc
