#include "aegra/adapters/windows_process/windows_process_launcher.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aegra::adapters::windows_process {
namespace {

struct HandleTraits {
    static void close(HANDLE handle) noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

class UniqueHandle final {
  public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) : handle_(h) {}
    ~UniqueHandle() { HandleTraits::close(handle_); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            HandleTraits::close(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE handle_{nullptr};
};

class EnvironmentStrings final {
  public:
    EnvironmentStrings() : value_(GetEnvironmentStringsW()) {}
    ~EnvironmentStrings() {
        if (value_ != nullptr) {
            FreeEnvironmentStringsW(value_);
        }
    }

    EnvironmentStrings(const EnvironmentStrings&) = delete;
    EnvironmentStrings& operator=(const EnvironmentStrings&) = delete;

    [[nodiscard]] LPWCH get() const noexcept { return value_; }

  private:
    LPWCH value_{nullptr};
};

base::Result<std::wstring> utf8_to_utf16(const std::string_view utf8) {
    if (utf8.empty()) {
        return base::Result<std::wstring>::success({});
    }
    if (utf8.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return base::Result<std::wstring>::failure(
            {base::ErrorCode::kInvalidArgument, "UTF-8 process argument is too long"});
    }
    const auto input_size = static_cast<int>(utf8.size());
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_size, nullptr, 0);
    if (size == 0) {
        return base::Result<std::wstring>::failure(
            {base::ErrorCode::kInvalidArgument, "process argument is invalid UTF-8"});
    }
    std::wstring utf16(size, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_size, utf16.data(),
                            size) == 0) {
        return base::Result<std::wstring>::failure(
            {base::ErrorCode::kInvalidArgument, "process argument conversion failed"});
    }
    return base::Result<std::wstring>::success(std::move(utf16));
}

std::wstring quote_arg(const std::wstring_view argument) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2U + 1U, L'\\');
            result.push_back(character);
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2U, L'\\');
    result += L"\"";
    return result;
}

base::Result<std::wstring> build_command_line(const std::wstring_view executable,
                                              const std::vector<std::string>& args) {
    std::wstring command = quote_arg(executable);
    for (const auto& arg : args) {
        auto converted = utf8_to_utf16(arg);
        if (!converted)
            return base::Result<std::wstring>::failure(converted.error());
        command += L" ";
        command += quote_arg(converted.value());
    }
    return base::Result<std::wstring>::success(std::move(command));
}

std::wstring_view environment_name(const std::wstring_view entry) {
    const std::size_t search_from = !entry.empty() && entry.front() == L'=' ? 1 : 0;
    const std::size_t separator = entry.find(L'=', search_from);
    return separator == std::wstring_view::npos ? entry : entry.substr(0, separator);
}

bool environment_names_equal(const std::wstring_view left, const std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

base::Result<std::vector<std::wstring>>
convert_environment_overrides(const std::vector<ports::ProcessEnvironmentVariable>& overrides) {
    std::vector<std::wstring> converted;
    converted.reserve(overrides.size());
    for (const auto& variable : overrides) {
        if (variable.name.empty() || variable.name.find('=') != std::string::npos ||
            variable.name.find('\0') != std::string::npos ||
            variable.value.find('\0') != std::string::npos) {
            return base::Result<std::vector<std::wstring>>::failure(
                {base::ErrorCode::kInvalidArgument, "process environment override is invalid"});
        }
        auto name = utf8_to_utf16(variable.name);
        auto value = utf8_to_utf16(variable.value);
        if (!name || !value) {
            return base::Result<std::vector<std::wstring>>::failure(!name ? name.error()
                                                                          : value.error());
        }
        for (const auto& existing : converted) {
            if (environment_names_equal(environment_name(existing), name.value())) {
                return base::Result<std::vector<std::wstring>>::failure(
                    {base::ErrorCode::kInvalidArgument,
                     "process environment override is duplicated"});
            }
        }
        converted.push_back(name.value() + L"=" + value.value());
    }
    return base::Result<std::vector<std::wstring>>::success(std::move(converted));
}

base::Result<std::vector<wchar_t>>
build_environment_block(const std::vector<ports::ProcessEnvironmentVariable>& overrides) {
    if (overrides.empty()) {
        return base::Result<std::vector<wchar_t>>::success({});
    }
    auto converted = convert_environment_overrides(overrides);
    if (!converted) {
        return base::Result<std::vector<wchar_t>>::failure(converted.error());
    }

    EnvironmentStrings inherited;
    if (inherited.get() == nullptr) {
        return base::Result<std::vector<wchar_t>>::failure(
            {base::ErrorCode::kInternal, "GetEnvironmentStringsW failed"});
    }
    std::vector<std::wstring> entries;
    for (const wchar_t* cursor = inherited.get(); *cursor != L'\0';
         cursor += std::wcslen(cursor) + 1) {
        const std::wstring entry(cursor);
        const bool replaced =
            std::any_of(converted.value().begin(), converted.value().end(), [&](const auto& value) {
                return environment_names_equal(environment_name(entry), environment_name(value));
            });
        if (!replaced) {
            entries.push_back(entry);
        }
    }
    entries.insert(entries.end(), converted.value().begin(), converted.value().end());
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });

    std::vector<wchar_t> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return base::Result<std::vector<wchar_t>>::success(std::move(block));
}

// Console tools emit the OEM code page when redirected to a file.
std::string oem_bytes_to_utf8(const std::string_view bytes) {
    if (bytes.empty() ||
        bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const auto input_size = static_cast<int>(bytes.size());
    const int wide_size = MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), input_size, nullptr, 0);
    if (wide_size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    if (MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), input_size, wide.data(), wide_size) == 0) {
        return {};
    }
    const int utf8_size =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(utf8_size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, utf8.data(), utf8_size, nullptr,
                            nullptr) == 0) {
        return {};
    }
    return utf8;
}

// Inheritable temp file backing the child's stdout/stderr. A file (unlike a pipe) cannot
// deadlock the child when the parent only reads after exit; DELETE_ON_CLOSE cleans it up.
[[nodiscard]] UniqueHandle create_output_capture_file() {
    wchar_t temp_dir[MAX_PATH + 1]{};
    const DWORD dir_length = GetTempPathW(MAX_PATH + 1, temp_dir);
    if (dir_length == 0 || dir_length > MAX_PATH) {
        return {};
    }
    wchar_t temp_file[MAX_PATH + 1]{};
    if (GetTempFileNameW(temp_dir, L"aeg", 0, temp_file) == 0) {
        return {};
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    return UniqueHandle(CreateFileW(temp_file, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
}

constexpr DWORD kMaxCapturedOutputBytes = 16U * 1024U;

[[nodiscard]] std::string read_captured_output(const HANDLE file) {
    LARGE_INTEGER begin{};
    if (SetFilePointerEx(file, begin, nullptr, FILE_BEGIN) == FALSE) {
        return {};
    }
    std::string raw(kMaxCapturedOutputBytes, '\0');
    DWORD read_bytes = 0;
    if (ReadFile(file, raw.data(), kMaxCapturedOutputBytes, &read_bytes, nullptr) == FALSE) {
        return {};
    }
    raw.resize(read_bytes);
    return oem_bytes_to_utf8(raw);
}

} // namespace

struct ProcessState final {
    explicit ProcessState(UniqueHandle value) : handle(std::move(value)) {}
    UniqueHandle handle;
    UniqueHandle output_file;
    std::atomic<bool> waiter_active{false};
    std::atomic<bool> termination_requested{false};
};

struct WindowsProcessLauncher::Impl final {
    std::mutex mutex;
    std::unordered_map<std::uint32_t, std::shared_ptr<ProcessState>> processes;
};

WindowsProcessLauncher::WindowsProcessLauncher() : impl_(std::make_unique<Impl>()) {}

WindowsProcessLauncher::~WindowsProcessLauncher() {
    std::vector<std::shared_ptr<ProcessState>> states;
    {
        std::lock_guard lock(impl_->mutex);
        for (const auto& [pid, state] : impl_->processes)
            states.push_back(state);
        impl_->processes.clear();
    }
    for (const auto& state : states) {
        state->termination_requested = true;
        (void)TerminateProcess(state->handle.get(), 1);
        (void)WaitForSingleObject(state->handle.get(), 5'000);
    }
}

base::Result<ports::ProcessLaunchResult>
WindowsProcessLauncher::launch(const ports::ProcessLaunchRequest& request) {
    auto executable = utf8_to_utf16(request.executable_path);
    if (!executable || executable.value().empty()) {
        return base::Result<ports::ProcessLaunchResult>::failure(
            executable ? base::Error{base::ErrorCode::kInvalidArgument, "process path is empty"}
                       : executable.error());
    }
    auto command = build_command_line(executable.value(), request.arguments);
    if (!command)
        return base::Result<ports::ProcessLaunchResult>::failure(command.error());
    auto environment = build_environment_block(request.environment_overrides);
    if (!environment) {
        return base::Result<ports::ProcessLaunchResult>::failure(environment.error());
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    UniqueHandle output_file;
    BOOL inherit_handles = FALSE;
    if (request.capture_output) {
        output_file = create_output_capture_file();
        if (output_file) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = output_file.get();
            si.hStdError = output_file.get();
            si.hStdInput = INVALID_HANDLE_VALUE;
            inherit_handles = TRUE;
        }
        // Capture setup failure is non-fatal; the process runs without capture.
    }

    DWORD creation_flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    void* environment_data = nullptr;
    if (!environment.value().empty()) {
        creation_flags |= CREATE_UNICODE_ENVIRONMENT;
        environment_data = environment.value().data();
    }

    if (!CreateProcessW(executable.value().c_str(), command.value().data(), nullptr, nullptr,
                        inherit_handles, creation_flags, environment_data, nullptr, &si, &pi)) {
        return base::Result<ports::ProcessLaunchResult>::failure(
            base::Error{base::ErrorCode::kInternal, "CreateProcessW failed"});
    }

    CloseHandle(pi.hThread);
    UniqueHandle process_handle(pi.hProcess);

    std::lock_guard lock(impl_->mutex);
    auto state = std::make_shared<ProcessState>(std::move(process_handle));
    state->output_file = std::move(output_file);
    impl_->processes[pi.dwProcessId] = std::move(state);

    return base::Result<ports::ProcessLaunchResult>::success(
        ports::ProcessLaunchResult{pi.dwProcessId});
}

base::Result<ports::ProcessExitStatus>
WindowsProcessLauncher::wait(std::uint32_t pid, const base::CancellationToken& cancellation) {
    std::shared_ptr<ProcessState> state;
    {
        std::lock_guard lock(impl_->mutex);
        const auto it = impl_->processes.find(pid);
        if (it == impl_->processes.end()) {
            return base::Result<ports::ProcessExitStatus>::failure(
                base::Error{base::ErrorCode::kNotFound, "Process not found"});
        }
        state = it->second;
    }
    if (state->waiter_active.exchange(true)) {
        return base::Result<ports::ProcessExitStatus>::failure(
            {base::ErrorCode::kConflict, "Process already has a waiter"});
    }
    struct WaiterGuard final {
        std::atomic<bool>& active;
        ~WaiterGuard() { active = false; }
    } waiter_guard{state->waiter_active};

    UniqueHandle event_handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event_handle) {
        return base::Result<ports::ProcessExitStatus>::failure(
            base::Error{base::ErrorCode::kInternal, "CreateEventW failed"});
    }

    std::stop_callback stop_cb(cancellation, [&]() { SetEvent(event_handle.get()); });

    HANDLE waits[2] = {state->handle.get(), event_handle.get()};
    const DWORD wait_result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);

    if (wait_result == WAIT_OBJECT_0 + 1) {
        return base::Result<ports::ProcessExitStatus>::failure(
            base::Error{base::ErrorCode::kCancelled, "Wait cancelled"});
    }

    if (wait_result != WAIT_OBJECT_0) {
        return base::Result<ports::ProcessExitStatus>::failure(
            base::Error{base::ErrorCode::kInternal, "WaitForMultipleObjects failed"});
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(state->handle.get(), &exit_code)) {
        return base::Result<ports::ProcessExitStatus>::failure(
            {base::ErrorCode::kInternal, "GetExitCodeProcess failed"});
    }

    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->processes.find(pid);
        if (found != impl_->processes.end() && found->second == state) {
            impl_->processes.erase(found);
        }
    }

    ports::ProcessExitStatus status;
    status.exit_code = exit_code;
    status.terminated = state->termination_requested;
    if (state->output_file) {
        status.output = read_captured_output(state->output_file.get());
    }

    return base::Result<ports::ProcessExitStatus>::success(status);
}

base::Result<void> WindowsProcessLauncher::terminate(std::uint32_t pid) {
    std::shared_ptr<ProcessState> state;
    {
        std::lock_guard lock(impl_->mutex);
        const auto it = impl_->processes.find(pid);
        if (it == impl_->processes.end()) {
            return base::Result<void>::success();
        }
        state = it->second;
    }
    if (WaitForSingleObject(state->handle.get(), 0) == WAIT_OBJECT_0) {
        return base::Result<void>::success();
    }
    state->termination_requested = true;
    if (!TerminateProcess(state->handle.get(), 1)) {
        if (WaitForSingleObject(state->handle.get(), 0) == WAIT_OBJECT_0) {
            return base::Result<void>::success();
        }
        return base::Result<void>::failure({base::ErrorCode::kInternal, "TerminateProcess failed"});
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::windows_process
