#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"
#include "windows_boot_security.h"

#include <tbs.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace aegra::adapters::windows_disk {
namespace {

using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);

[[nodiscard]] base::Result<std::filesystem::path> windows_volume_guid_path() {
    std::array<wchar_t, MAX_PATH + 1> windows_directory{};
    const auto length = GetWindowsDirectoryW(windows_directory.data(),
                                             static_cast<UINT>(windows_directory.size()));
    if (length == 0 || length >= windows_directory.size()) {
        return base::Result<std::filesystem::path>::failure(
            detail::win32_error(GetLastError(), "GetWindowsDirectoryW"));
    }
    std::array<wchar_t, MAX_PATH + 1> mount_point{};
    if (!GetVolumePathNameW(windows_directory.data(), mount_point.data(),
                            static_cast<DWORD>(mount_point.size()))) {
        return base::Result<std::filesystem::path>::failure(
            detail::win32_error(GetLastError(), "GetVolumePathNameW"));
    }
    std::array<wchar_t, MAX_PATH + 1> volume_name{};
    if (!GetVolumeNameForVolumeMountPointW(mount_point.data(), volume_name.data(),
                                           static_cast<DWORD>(volume_name.size()))) {
        return base::Result<std::filesystem::path>::failure(
            detail::win32_error(GetLastError(), "GetVolumeNameForVolumeMountPointW"));
    }
    return base::Result<std::filesystem::path>::success(volume_name.data());
}

[[nodiscard]] base::Result<WindowsFirmwareMode> firmware_mode() {
    FIRMWARE_TYPE firmware = FirmwareTypeUnknown;
    if (!GetFirmwareType(&firmware)) {
        return base::Result<WindowsFirmwareMode>::failure(
            detail::win32_error(GetLastError(), "GetFirmwareType"));
    }
    if (firmware == FirmwareTypeUefi) {
        return base::Result<WindowsFirmwareMode>::success(WindowsFirmwareMode::kUefi);
    }
    if (firmware == FirmwareTypeBios) {
        return base::Result<WindowsFirmwareMode>::success(WindowsFirmwareMode::kBios);
    }
    return base::Result<WindowsFirmwareMode>::failure(
        {base::ErrorCode::kUnsupportedVersion, "Windows firmware mode is unsupported"});
}

[[nodiscard]] WindowsOsArchitecture native_architecture() noexcept {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    return info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64
               ? WindowsOsArchitecture::kX64
               : WindowsOsArchitecture::kUnsupported;
}

[[nodiscard]] base::Result<std::string> os_build() {
    const auto module = GetModuleHandleW(L"ntdll.dll");
    if (module == nullptr) {
        return base::Result<std::string>::failure(
            detail::win32_error(GetLastError(), "GetModuleHandleW ntdll"));
    }
    const auto function = reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(module, "RtlGetVersion"));
    if (function == nullptr) {
        return base::Result<std::string>::failure(
            detail::win32_error(GetLastError(), "GetProcAddress RtlGetVersion"));
    }
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (function(&version) != 0) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kIoFailure, "RtlGetVersion failed"});
    }
    return base::Result<std::string>::success(
        std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion) +
        "." + std::to_string(version.dwBuildNumber));
}

[[nodiscard]] WindowsSecurityState secure_boot_state(const WindowsFirmwareMode firmware) noexcept {
    if (firmware == WindowsFirmwareMode::kBios) {
        return WindowsSecurityState::kDisabled;
    }
    DWORD enabled = 0;
    DWORD size = sizeof(enabled);
    const auto status = RegGetValueW(
        HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
        L"UEFISecureBootEnabled", RRF_RT_REG_DWORD, nullptr, &enabled, &size);
    if (status != ERROR_SUCCESS || size != sizeof(enabled)) {
        return WindowsSecurityState::kUnknown;
    }
    return enabled == 0 ? WindowsSecurityState::kDisabled : WindowsSecurityState::kEnabled;
}

[[nodiscard]] WindowsHardwareState tpm_state() noexcept {
    TPM_DEVICE_INFO info{};
    info.structVersion = TPM_VERSION_20;
    const auto status = Tbsi_GetDeviceInfo(sizeof(info), &info);
    if (status == TBS_SUCCESS) {
        return WindowsHardwareState::kPresent;
    }
    if (status == TBS_E_TPM_NOT_FOUND) {
        return WindowsHardwareState::kAbsent;
    }
    return WindowsHardwareState::kUnknown;
}

} // namespace

base::Result<WindowsBootEnvironment> inspect_windows_boot_environment() {
    auto volume = windows_volume_guid_path();
    if (!volume) {
        return base::Result<WindowsBootEnvironment>::failure(volume.error());
    }
    auto firmware = firmware_mode();
    if (!firmware) {
        return base::Result<WindowsBootEnvironment>::failure(firmware.error());
    }
    auto build = os_build();
    if (!build) {
        return base::Result<WindowsBootEnvironment>::failure(build.error());
    }

    WindowsBootEnvironment environment;
    environment.windows_volume_guid_path = std::move(volume).value();
    environment.firmware_mode = firmware.value();
    environment.os_architecture = native_architecture();
    environment.os_build = std::move(build).value();
    environment.secure_boot_state = secure_boot_state(environment.firmware_mode);
    environment.tpm_state = tpm_state();
    environment.bitlocker_state =
        detail::inspect_bitlocker_state(environment.windows_volume_guid_path);
    return base::Result<WindowsBootEnvironment>::success(std::move(environment));
}

} // namespace aegra::adapters::windows_disk
