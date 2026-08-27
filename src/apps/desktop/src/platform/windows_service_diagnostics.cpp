#include "platform/windows_service_diagnostics.h"

#if defined(Q_OS_WIN)
#include <Windows.h>
#include <shellapi.h>

#include <array>
#include <string>
#endif

#include <utility>

namespace aegra::desktop {
namespace {

#if defined(Q_OS_WIN)
constexpr auto kServiceName = L"AegraService";

class UniqueScHandle final {
  public:
    explicit UniqueScHandle(const SC_HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueScHandle() {
        if (handle_ != nullptr) {
            CloseServiceHandle(handle_);
        }
    }

    UniqueScHandle(const UniqueScHandle&) = delete;
    UniqueScHandle& operator=(const UniqueScHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] SC_HANDLE get() const noexcept { return handle_; }

  private:
    SC_HANDLE handle_{nullptr};
};

[[nodiscard]] bool request_elevated_start() {
    std::array<wchar_t, MAX_PATH> system_directory{};
    const auto length =
        GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) {
        return false;
    }
    std::wstring executable(system_directory.data(), length);
    executable.append(L"\\sc.exe");
    SHELLEXECUTEINFOW request{};
    request.cbSize = sizeof(request);
    request.fMask = SEE_MASK_FLAG_NO_UI;
    request.lpVerb = L"runas";
    request.lpFile = executable.c_str();
    request.lpParameters = L"start AegraService";
    request.nShow = SW_HIDE;
    return ShellExecuteExW(&request) != FALSE;
}

[[nodiscard]] QString start_stopped_service(const SC_HANDLE manager) {
    UniqueScHandle service(OpenServiceW(manager, kServiceName, SERVICE_START));
    if (!service.valid()) {
        if (GetLastError() == ERROR_ACCESS_DENIED && request_elevated_start()) {
            return QStringLiteral("aegra.service.diagnostic.elevation_requested");
        }
        return QStringLiteral("aegra.service.diagnostic.start_failed");
    }
    if (StartServiceW(service.get(), 0, nullptr) != FALSE) {
        return QStringLiteral("aegra.service.diagnostic.start_requested");
    }
    const auto error = GetLastError();
    if (error == ERROR_SERVICE_ALREADY_RUNNING) {
        return QStringLiteral("aegra.service.diagnostic.running");
    }
    if (error == ERROR_ACCESS_DENIED && request_elevated_start()) {
        return QStringLiteral("aegra.service.diagnostic.elevation_requested");
    }
    return QStringLiteral("aegra.service.diagnostic.start_failed");
}

[[nodiscard]] QString diagnose_windows_service() {
    UniqueScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager.valid()) {
        return QStringLiteral("aegra.service.diagnostic.query_failed");
    }
    UniqueScHandle service(OpenServiceW(manager.get(), kServiceName, SERVICE_QUERY_STATUS));
    if (!service.valid()) {
        return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST
                   ? QStringLiteral("aegra.service.diagnostic.not_installed")
                   : QStringLiteral("aegra.service.diagnostic.query_failed");
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes_needed = 0;
    if (QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<LPBYTE>(&status), sizeof(status),
                             &bytes_needed) == FALSE) {
        return QStringLiteral("aegra.service.diagnostic.query_failed");
    }
    if (status.dwCurrentState == SERVICE_STOPPED) {
        return start_stopped_service(manager.get());
    }
    if (status.dwCurrentState == SERVICE_STOP_PENDING) {
        return QStringLiteral("aegra.service.diagnostic.stopping");
    }
    if (status.dwCurrentState == SERVICE_START_PENDING) {
        return QStringLiteral("aegra.service.diagnostic.start_requested");
    }
    return QStringLiteral("aegra.service.diagnostic.running");
}
#endif

} // namespace

WindowsServiceDiagnostics::WindowsServiceDiagnostics(QObject* parent) : QObject(parent) {}

QString WindowsServiceDiagnostics::messageId() const { return message_id_; }

void WindowsServiceDiagnostics::diagnose() {
#if defined(Q_OS_WIN)
    set_message_id(diagnose_windows_service());
#else
    set_message_id(QStringLiteral("aegra.service.diagnostic.query_failed"));
#endif
}

void WindowsServiceDiagnostics::reset() { set_message_id({}); }

void WindowsServiceDiagnostics::set_message_id(QString message_id) {
    if (message_id_ == message_id) {
        return;
    }
    message_id_ = std::move(message_id);
    emit changed();
}

} // namespace aegra::desktop
