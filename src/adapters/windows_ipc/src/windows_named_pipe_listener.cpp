#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"

#include "windows_named_pipe_security_internal.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::adapters::windows_ipc {
namespace {

constexpr std::uint32_t kMaximumListenerFrameBytes = 1024U * 1024U;

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

[[nodiscard]] bool valid_pipe_name(const std::string_view name) noexcept {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](const unsigned char value) {
        const bool alpha_numeric = (value >= 'a' && value <= 'z') ||
                                   (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
        return alpha_numeric || value == '-' || value == '_' || value == '.';
    });
}

[[nodiscard]] bool valid_acl_profile(const WindowsNamedPipeAclProfile profile) noexcept {
    return profile == WindowsNamedPipeAclProfile::kProcessDefault ||
           profile == WindowsNamedPipeAclProfile::kServiceLocalControl;
}

[[nodiscard]] std::wstring service_pipe_path(const std::string_view name) {
    std::wstring result = LR"(\\.\pipe\aegra-service-)";
    result.reserve(result.size() + name.size());
    for (const char value : name) {
        result.push_back(static_cast<wchar_t>(value));
    }
    return result;
}

[[nodiscard]] base::Error accept_error(const DWORD code) {
    if (code == ERROR_OPERATION_ABORTED) {
        return {base::ErrorCode::kCancelled, "named pipe accept cancelled"};
    }
    if (code == ERROR_ACCESS_DENIED || code == ERROR_PIPE_BUSY) {
        return {base::ErrorCode::kConflict, "named pipe listener is unavailable"};
    }
    return {base::ErrorCode::kIoFailure, "named pipe accept failed"};
}

struct CancelPendingConnect final {
    HANDLE pipe;
    OVERLAPPED* overlapped;

    void operator()() const noexcept { CancelIoEx(pipe, overlapped); }
};

[[nodiscard]] base::Result<void> wait_for_client(const HANDLE pipe,
                                                 const base::CancellationToken& cancellation) {
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInternal, "named pipe event creation failed"});
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &overlapped)) {
        return base::Result<void>::success();
    }
    const auto start_error = GetLastError();
    if (start_error == ERROR_PIPE_CONNECTED) {
        return base::Result<void>::success();
    }
    if (start_error != ERROR_IO_PENDING) {
        return base::Result<void>::failure(accept_error(start_error));
    }
    std::stop_callback cancel(cancellation, CancelPendingConnect{pipe, &overlapped});
    (void)WaitForSingleObject(event.get(), INFINITE);
    DWORD transferred = 0;
    if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
        return base::Result<void>::failure(accept_error(GetLastError()));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<UniqueHandle>
create_service_pipe(const std::wstring& path, const std::uint32_t buffer_bytes,
                    const WindowsNamedPipeAclProfile acl_profile,
                    detail::UniqueLocal& security_owner, SECURITY_ATTRIBUTES& security_attributes) {
    LPSECURITY_ATTRIBUTES security = nullptr;
    if (acl_profile == WindowsNamedPipeAclProfile::kServiceLocalControl) {
        auto created =
            detail::create_service_local_security_attributes(security_attributes, security_owner);
        if (!created) {
            return base::Result<UniqueHandle>::failure(created.error());
        }
        security = created.value();
    }
    UniqueHandle pipe(CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                           PIPE_REJECT_REMOTE_CLIENTS,
                                       PIPE_UNLIMITED_INSTANCES, buffer_bytes, buffer_bytes, 0,
                                       security));
    if (!pipe.valid()) {
        return base::Result<UniqueHandle>::failure(accept_error(GetLastError()));
    }
    return base::Result<UniqueHandle>::success(std::move(pipe));
}

} // namespace

WindowsNamedPipeListener::WindowsNamedPipeListener(WindowsNamedPipeListenRequest request)
    : request_(std::move(request)) {}

WindowsNamedPipeListener::~WindowsNamedPipeListener() = default;

base::Result<std::unique_ptr<WindowsNamedPipeListener>>
WindowsNamedPipeListener::create(const WindowsNamedPipeListenRequest& request) {
    if (!valid_pipe_name(request.pipe_name) || request.maximum_frame_bytes == 0 ||
        request.maximum_frame_bytes > kMaximumListenerFrameBytes ||
        !valid_acl_profile(request.acl_profile)) {
        return base::Result<std::unique_ptr<WindowsNamedPipeListener>>::failure(base::Error{
            base::ErrorCode::kInvalidArgument, "named pipe listener request is invalid"});
    }
    return base::Result<std::unique_ptr<WindowsNamedPipeListener>>::success(
        std::unique_ptr<WindowsNamedPipeListener>(new WindowsNamedPipeListener(request)));
}

base::Result<std::unique_ptr<WindowsNamedPipeChannel>>
WindowsNamedPipeListener::accept(const base::CancellationToken& cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::unique_ptr<WindowsNamedPipeChannel>>::failure(
            base::Error{base::ErrorCode::kCancelled, "named pipe accept cancelled"});
    }
    const auto path = service_pipe_path(request_.pipe_name);
    detail::UniqueLocal security_owner;
    SECURITY_ATTRIBUTES security_attributes{};
    auto pipe = create_service_pipe(path, request_.maximum_frame_bytes, request_.acl_profile,
                                    security_owner, security_attributes);
    if (!pipe) {
        return base::Result<std::unique_ptr<WindowsNamedPipeChannel>>::failure(pipe.error());
    }
    auto connected = wait_for_client(pipe.value().get(), cancellation);
    if (!connected) {
        return base::Result<std::unique_ptr<WindowsNamedPipeChannel>>::failure(connected.error());
    }
    const auto native_handle = reinterpret_cast<std::uintptr_t>(pipe.value().release());
    return base::Result<std::unique_ptr<WindowsNamedPipeChannel>>::success(
        WindowsNamedPipeChannel::adopt_connected(native_handle, request_.maximum_frame_bytes));
}

base::Result<WindowsNamedPipeAcceptedClient>
WindowsNamedPipeListener::accept_authorized(const WindowsServiceCallerAuthorization& authorization,
                                            const base::CancellationToken& cancellation) {
    auto channel = accept(cancellation);
    if (!channel) {
        return base::Result<WindowsNamedPipeAcceptedClient>::failure(channel.error());
    }
    auto peer = channel.value()->peer_identity();
    if (!peer) {
        return base::Result<WindowsNamedPipeAcceptedClient>::failure(peer.error());
    }
    auto authorized = authorize_service_caller(peer.value(), authorization);
    if (!authorized) {
        return base::Result<WindowsNamedPipeAcceptedClient>::failure(authorized.error());
    }
    WindowsNamedPipeAcceptedClient accepted;
    accepted.channel = std::move(channel).value();
    accepted.peer = std::move(peer).value();
    return base::Result<WindowsNamedPipeAcceptedClient>::success(std::move(accepted));
}

} // namespace aegra::adapters::windows_ipc
