#include "aegra/apps/boot_check/boot_check_host.h"

#include "aegra/adapters/hyperv/hyperv_discovery.h"
#include "aegra/adapters/virtualbox/virtualbox_discovery.h"
#include "aegra/adapters/windows_process/windows_process_launcher.h"
#include "aegra/adapters/windows_system/windows_system.h"

#include <Windows.h>
#include <shellapi.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace {

constexpr std::size_t kMaximumRequestBytes = std::size_t{1024} * 1024U;
constexpr std::string_view kStaticHostFailure =
    R"({"schema_version":1,"job_id":"","trace_id":"","kind":3,"boundary_error_code":9,"message_code":"bootcheck.host_failed","task_result":null})";

std::wstring environment_value(const wchar_t* const name) {
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD written =
        GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        return {};
    }
    buffer.resize(written);
    return buffer;
}

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const auto input_size = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                              input_size, nullptr, 0, nullptr, nullptr);
    if (required == 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size, output.data(),
                            required, nullptr, nullptr) == 0) {
        return {};
    }
    return output;
}

/// Mirrors the task-log data-dir order: AEGRA_DATA_DIR, LOCALAPPDATA, ProgramData.
std::wstring data_directory() {
    if (auto configured = environment_value(L"AEGRA_DATA_DIR"); !configured.empty()) {
        return configured;
    }
    if (auto local = environment_value(L"LOCALAPPDATA"); !local.empty()) {
        return local + L"\\Aegra";
    }
    if (auto program_data = environment_value(L"ProgramData"); !program_data.empty()) {
        return program_data + L"\\Aegra";
    }
    return {};
}

/// True when this process runs as LocalSystem. The Service launches BootCheck
/// under the logged-on user's token when one exists, so a non-System token here
/// means "user-visible run": the VirtualBox job VM should land in the user's own
/// default registry rather than a per-job isolated one.
bool current_process_is_local_system() {
    HANDLE raw = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) == FALSE) {
        return true; // Fail safe: assume System, keep the isolated registry.
    }
    std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&CloseHandle)> token(raw, &CloseHandle);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (size == 0) {
        return true;
    }
    std::vector<std::byte> buffer(size);
    if (GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) == FALSE) {
        return true;
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID system_sid = nullptr;
    if (AllocateAndInitializeSid(&authority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0,
                                 &system_sid) == FALSE) {
        return true;
    }
    const bool is_system = EqualSid(user->User.Sid, system_sid) == TRUE;
    FreeSid(system_sid);
    return is_system;
}

std::string capability_user_home() {
    const auto data_dir = data_directory();
    if (data_dir.empty()) {
        return {};
    }
    const std::filesystem::path home =
        std::filesystem::path(data_dir) / L"bootcheck" / L"capability";
    std::error_code error;
    std::filesystem::create_directories(home, error);
    if (error) {
        return {};
    }
    return to_utf8(home.native());
}

std::string read_request() {
    std::string request;
    request.reserve(4096);
    std::array<char, 4096> buffer{};
    while (std::cin.good() && request.size() <= kMaximumRequestBytes) {
        std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        request.append(buffer.data(), static_cast<std::size_t>(std::cin.gcount()));
    }
    return request;
}

/// Supervisor mode: the Service cannot feed a child's stdin, so it stages the
/// request JSON in a private file. An unreadable or oversized file yields an
/// empty request, which the host rejects like malformed stdin.
std::string read_request_file(const wchar_t* const path) {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string request;
    std::array<char, 4096> buffer{};
    DWORD read_bytes = 0;
    while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read_bytes, nullptr) &&
           read_bytes > 0 && request.size() <= kMaximumRequestBytes) {
        request.append(buffer.data(), read_bytes);
    }
    CloseHandle(file);
    return request;
}

[[nodiscard]] std::uint32_t parse_hold_minutes(const std::span<const char* const> arguments) {
    constexpr std::uint32_t kDefaultHoldMinutes = 60;
    constexpr std::uint32_t kMaximumHoldMinutes = 24 * 60;
    if (arguments.size() < 3) {
        return kDefaultHoldMinutes;
    }
    const std::string_view text(arguments[2]);
    std::uint32_t minutes = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), minutes);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || minutes == 0 ||
        minutes > kMaximumHoldMinutes) {
        return 0;
    }
    return minutes;
}

/// Wide argv lookup for path arguments (the narrow argv loses non-ASCII paths).
std::wstring wide_argument(const int index) {
    int count = 0;
    wchar_t** const arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) {
        return {};
    }
    std::wstring value = index < count ? arguments[index] : L"";
    LocalFree(arguments);
    return value;
}

int run_host(const std::span<const char* const> arguments) {
    const bool present_only =
        arguments.size() >= 2 && std::string_view(arguments[1]) == "--present-only";
    const bool request_file_mode =
        arguments.size() == 3 && std::string_view(arguments[1]) == "--request";
    const bool scavenge_mode =
        arguments.size() == 2 && std::string_view(arguments[1]) == "--scavenge";
    const bool inspect_mode =
        arguments.size() == 3 && std::string_view(arguments[1]) == "--inspect";
    if (!(arguments.size() == 1 || request_file_mode || scavenge_mode || inspect_mode ||
          (present_only && arguments.size() <= 3))) {
        return static_cast<int>(aegra::apps::boot_check::BootCheckExitCode::kRequestRejected);
    }
    const std::uint32_t hold_minutes = present_only ? parse_hold_minutes(arguments) : 0;
    if (present_only && hold_minutes == 0) {
        return static_cast<int>(aegra::apps::boot_check::BootCheckExitCode::kRequestRejected);
    }
    aegra::adapters::windows_system::WindowsCredentialResolver credentials;
    aegra::adapters::windows_system::WindowsCryptographicRandom random;
    aegra::adapters::windows_system::WindowsSystemClock clock;
    aegra::adapters::windows_process::WindowsProcessLauncher launcher;

    aegra::apps::boot_check::BootCheckHostOptions options;
    options.vbox_manage_path = aegra::adapters::virtualbox::discover_vbox_manage_path();
    options.capability_user_home = capability_user_home();
    options.powershell_path = aegra::adapters::hyperv::discover_powershell_path();
    // A non-System token means the Service started us under the logged-on user,
    // so register the job VM in that user's default VirtualBox registry.
    options.use_isolated_vbox_home = current_process_is_local_system();

    const aegra::apps::boot_check::BootCheckHostContext context{credentials, random, clock,
                                                                launcher};
    if (scavenge_mode) {
        return static_cast<int>(aegra::apps::boot_check::run_boot_check_scavenge(
            options, context, std::filesystem::path(data_directory())));
    }
    if (inspect_mode) {
        auto inspected =
            aegra::apps::boot_check::run_boot_check_inspect(arguments[2], options, context);
        if (!inspected) {
            return static_cast<int>(aegra::apps::boot_check::BootCheckExitCode::kRequestRejected);
        }
        std::cout << inspected.value().response_json << '\n';
        return static_cast<int>(inspected.value().exit_code);
    }
    const std::string encoded_request =
        request_file_mode ? read_request_file(wide_argument(2).c_str()) : read_request();
    auto result = present_only ? aegra::apps::boot_check::run_boot_check_present_request(
                                     encoded_request, options, context, hold_minutes, {})
                               : aegra::apps::boot_check::run_boot_check_host_request(
                                     encoded_request, options, context, {});
    if (!result) {
        return static_cast<int>(aegra::apps::boot_check::BootCheckExitCode::kHostFailure);
    }
    std::cout << result.value().response_json << '\n';
    return static_cast<int>(result.value().exit_code);
}

} // namespace

int main(const int argument_count, const char* const* arguments) noexcept {
    try {
        return run_host({arguments, static_cast<std::size_t>(argument_count)});
    } catch (...) {
        const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        WriteFile(output, kStaticHostFailure.data(), static_cast<DWORD>(kStaticHostFailure.size()),
                  &written, nullptr);
        return static_cast<int>(aegra::apps::boot_check::BootCheckExitCode::kHostFailure);
    }
}
