#include "aegra/apps/service/windows_service_control.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

constexpr std::uint32_t kStopWaitDeadlineMs = 30'000;
constexpr std::uint32_t kStopPollIntervalMs = 100;

class UniqueScHandle final {
  public:
    explicit UniqueScHandle(const SC_HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~UniqueScHandle() {
        if (handle_ != nullptr) {
            CloseServiceHandle(handle_);
        }
    }

    UniqueScHandle(const UniqueScHandle&) = delete;
    UniqueScHandle& operator=(const UniqueScHandle&) = delete;
    UniqueScHandle(UniqueScHandle&& other) noexcept : handle_(other.release()) {}
    UniqueScHandle& operator=(UniqueScHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) {
                CloseServiceHandle(handle_);
            }
            handle_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] SC_HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] SC_HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

  private:
    SC_HANDLE handle_{nullptr};
};

[[nodiscard]] std::wstring widen_utf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const auto needed =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), wide.data(), needed) <= 0) {
        return {};
    }
    return wide;
}

[[nodiscard]] std::string narrow_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const auto needed =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string narrow(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), narrow.data(), needed, nullptr,
                            nullptr) <= 0) {
        return {};
    }
    return narrow;
}

[[nodiscard]] base::Error scm_error(const char* message) {
    const auto code = GetLastError();
    if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
        return {base::ErrorCode::kNotFound, message};
    }
    if (code == ERROR_SERVICE_EXISTS || code == ERROR_SERVICE_ALREADY_RUNNING ||
        code == ERROR_SERVICE_NOT_ACTIVE) {
        return {base::ErrorCode::kConflict, message};
    }
    if (code == ERROR_ACCESS_DENIED) {
        return {base::ErrorCode::kUnauthorized, message};
    }
    return {base::ErrorCode::kIoFailure, message};
}

[[nodiscard]] base::Result<void> query_status(const SC_HANDLE service,
                                              SERVICE_STATUS_PROCESS& status) {
    DWORD bytes_needed = 0;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status),
                             sizeof(status), &bytes_needed) == FALSE) {
        return base::Result<void>::failure(scm_error("windows service status query failed"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> wait_until_stopped(const SC_HANDLE service,
                                                    const std::uint32_t deadline_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    SERVICE_STATUS_PROCESS status{};
    for (;;) {
        auto queried = query_status(service, status);
        if (!queried) {
            return queried;
        }
        if (status.dwCurrentState == SERVICE_STOPPED) {
            return base::Result<void>::success();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return base::Result<void>::failure(
                base::Error{base::ErrorCode::kInternal, "windows service stop deadline exceeded"});
        }
        auto sleep_ms = kStopPollIntervalMs;
        if (status.dwCurrentState == SERVICE_STOP_PENDING && status.dwWaitHint > 0) {
            const auto hint_ms = static_cast<std::uint32_t>(status.dwWaitHint);
            sleep_ms = (std::min)(hint_ms, kStopPollIntervalMs * 10U);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
}

class WindowsServiceControlManager final : public IWindowsServiceControlManager {
  public:
    explicit WindowsServiceControlManager(UniqueScHandle manager) : manager_(std::move(manager)) {}

    [[nodiscard]] base::Result<void>
    create_service(const WindowsServiceInstallRequest& request) override {
        const auto name = widen_utf8(request.service_name);
        const auto display = widen_utf8(request.display_name);
        const auto binary = widen_utf8(request.binary_path);
        if (name.empty() || display.empty() || binary.empty()) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kInvalidArgument, "windows service install path is invalid"});
        }
        UniqueScHandle service(CreateServiceW(
            manager_.get(), name.c_str(), display.c_str(),
            SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            binary.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
        if (!service.valid()) {
            return base::Result<void>::failure(scm_error("windows service create failed"));
        }
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<void> delete_service(const std::string& service_name) override {
        auto service = open_service(service_name, DELETE | SERVICE_QUERY_STATUS);
        if (!service) {
            return base::Result<void>::failure(service.error());
        }
        if (DeleteService(service.value().get()) == FALSE) {
            return base::Result<void>::failure(scm_error("windows service delete failed"));
        }
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<void>
    configure_recovery(const std::string& service_name, const std::uint32_t recovery_delay_ms,
                       const std::uint32_t recovery_reset_period_seconds) override {
        auto service = open_service(service_name, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG);
        if (!service) {
            return base::Result<void>::failure(service.error());
        }
        std::array<SC_ACTION, 3> actions{};
        actions[0] = {SC_ACTION_RESTART, recovery_delay_ms};
        actions[1] = {SC_ACTION_RESTART, recovery_delay_ms};
        actions[2] = {SC_ACTION_NONE, 0};
        SERVICE_FAILURE_ACTIONSW failure{};
        failure.dwResetPeriod = recovery_reset_period_seconds;
        failure.cActions = static_cast<DWORD>(actions.size());
        failure.lpsaActions = actions.data();
        if (ChangeServiceConfig2W(service.value().get(), SERVICE_CONFIG_FAILURE_ACTIONS,
                                  &failure) == FALSE) {
            return base::Result<void>::failure(
                scm_error("windows service recovery configuration failed"));
        }
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<void> start_service(const std::string& service_name) override {
        auto service = open_service(service_name, SERVICE_START | SERVICE_QUERY_STATUS);
        if (!service) {
            return base::Result<void>::failure(service.error());
        }
        if (StartServiceW(service.value().get(), 0, nullptr) == FALSE) {
            return base::Result<void>::failure(scm_error("windows service start failed"));
        }
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<void> stop_service(const std::string& service_name) override {
        auto service = open_service(service_name, SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (!service) {
            return base::Result<void>::failure(service.error());
        }
        SERVICE_STATUS_PROCESS status{};
        auto queried = query_status(service.value().get(), status);
        if (!queried) {
            return queried;
        }
        if (status.dwCurrentState == SERVICE_STOPPED) {
            return base::Result<void>::success();
        }
        if (status.dwCurrentState != SERVICE_STOP_PENDING) {
            SERVICE_STATUS control_status{};
            if (ControlService(service.value().get(), SERVICE_CONTROL_STOP, &control_status) ==
                FALSE) {
                const auto code = GetLastError();
                if (code != ERROR_SERVICE_NOT_ACTIVE) {
                    return base::Result<void>::failure(scm_error("windows service stop failed"));
                }
            }
        }
        return wait_until_stopped(service.value().get(), kStopWaitDeadlineMs);
    }

    [[nodiscard]] base::Result<WindowsServiceIdentity>
    query_service(const std::string& service_name) const override {
        auto service =
            open_service(service_name, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
        if (!service) {
            return base::Result<WindowsServiceIdentity>::failure(service.error());
        }
        DWORD needed = 0;
        (void)QueryServiceConfigW(service.value().get(), nullptr, 0, &needed);
        if (needed == 0) {
            return base::Result<WindowsServiceIdentity>::failure(
                scm_error("windows service query failed"));
        }
        std::vector<std::byte> buffer(needed);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (QueryServiceConfigW(service.value().get(), config, needed, &needed) == FALSE) {
            return base::Result<WindowsServiceIdentity>::failure(
                scm_error("windows service query failed"));
        }
        WindowsServiceIdentity identity;
        identity.service_name = service_name;
        identity.display_name = narrow_utf8(config->lpDisplayName != nullptr ? config->lpDisplayName
                                                                             : L"");
        identity.binary_path =
            narrow_utf8(config->lpBinaryPathName != nullptr ? config->lpBinaryPathName : L"");

        DWORD failure_needed = 0;
        (void)QueryServiceConfig2W(service.value().get(), SERVICE_CONFIG_FAILURE_ACTIONS, nullptr, 0,
                                   &failure_needed);
        if (failure_needed > 0) {
            std::vector<std::byte> failure_buffer(failure_needed);
            if (QueryServiceConfig2W(service.value().get(), SERVICE_CONFIG_FAILURE_ACTIONS,
                                     reinterpret_cast<LPBYTE>(failure_buffer.data()), failure_needed,
                                     &failure_needed) != FALSE) {
                const auto* failure =
                    reinterpret_cast<const SERVICE_FAILURE_ACTIONSW*>(failure_buffer.data());
                if (failure->cActions > 0 && failure->lpsaActions != nullptr &&
                    failure->lpsaActions[0].Type == SC_ACTION_RESTART) {
                    identity.recovery_enabled = true;
                    identity.recovery_delay_ms = failure->lpsaActions[0].Delay;
                }
            }
        }
        return base::Result<WindowsServiceIdentity>::success(std::move(identity));
    }

  private:
    [[nodiscard]] base::Result<UniqueScHandle> open_service(const std::string& service_name,
                                                            const DWORD access) const {
        const auto name = widen_utf8(service_name);
        if (name.empty()) {
            return base::Result<UniqueScHandle>::failure(
                base::Error{base::ErrorCode::kInvalidArgument, "windows service name is invalid"});
        }
        UniqueScHandle service(OpenServiceW(manager_.get(), name.c_str(), access));
        if (!service.valid()) {
            return base::Result<UniqueScHandle>::failure(scm_error("windows service open failed"));
        }
        return base::Result<UniqueScHandle>::success(std::move(service));
    }

    UniqueScHandle manager_;
};

} // namespace

base::Result<std::unique_ptr<IWindowsServiceControlManager>>
open_windows_service_control_manager() {
    UniqueScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    if (!manager.valid()) {
        return base::Result<std::unique_ptr<IWindowsServiceControlManager>>::failure(
            scm_error("windows service control manager open failed"));
    }
    return base::Result<std::unique_ptr<IWindowsServiceControlManager>>::success(
        std::make_unique<WindowsServiceControlManager>(std::move(manager)));
}

} // namespace aegra::apps::service
