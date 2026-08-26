#include "pe_boot_internal.h"

#include "pe_pending_internal.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace aegra::adapters::windows_pe::detail {
namespace {

inline constexpr std::size_t kMaximumOutputExcerpt = 300;

[[nodiscard]] base::Result<std::string> utf16_to_utf8(const std::wstring_view value) {
    if (value.empty()) {
        return base::Result<std::string>::success(std::string());
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return base::Result<std::string>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "path is too long"));
    }
    const auto input_size = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                              input_size, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return base::Result<std::string>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "path is not valid utf-16"));
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size, utf8.data(),
                            required, nullptr, nullptr) != required) {
        return base::Result<std::string>::failure(
            win32_error(GetLastError(), "convert system path"));
    }
    return base::Result<std::string>::success(std::move(utf8));
}

} // namespace

base::Result<std::string> resolve_system_tool_path(const std::string_view tool_name) {
    wchar_t system_directory[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return base::Result<std::string>::failure(
            win32_error(GetLastError(), "resolve system directory"));
    }
    auto utf8 = utf16_to_utf8(std::wstring_view(system_directory, length));
    if (!utf8) {
        return utf8;
    }
    std::string path = std::move(utf8).value();
    path += '\\';
    path += tool_name;
    return base::Result<std::string>::success(std::move(path));
}

base::Result<void> require_elevated() {
    HANDLE token_raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_raw)) {
        return base::Result<void>::failure(win32_error(GetLastError(), "open process token"));
    }
    const UniqueHandle token(token_raw);
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation),
                             &returned)) {
        return base::Result<void>::failure(win32_error(GetLastError(), "query token elevation"));
    }
    if (elevation.TokenIsElevated == 0) {
        return base::Result<void>::failure(pe_store_error(
            base::ErrorCode::kUnauthorized,
            "administrator privileges are required for boot and image operations"));
    }
    return base::Result<void>::success();
}

base::Result<ConsoleToolResult> run_console_tool(ports::IProcessLauncher& launcher,
                                                 const std::string& tool_path_utf8,
                                                 std::vector<std::string> arguments) {
    ports::ProcessLaunchRequest launch;
    launch.executable_path = tool_path_utf8;
    launch.arguments = std::move(arguments);
    launch.capture_output = true;
    auto started = launcher.launch(launch);
    if (!started) {
        return base::Result<ConsoleToolResult>::failure(started.error());
    }
    auto exited = launcher.wait(started.value().pid, base::CancellationToken{});
    if (!exited) {
        return base::Result<ConsoleToolResult>::failure(exited.error());
    }
    ConsoleToolResult result;
    result.exit_code = exited.value().exit_code;
    result.terminated = exited.value().terminated;
    result.output = std::move(exited.value().output);
    return base::Result<ConsoleToolResult>::success(std::move(result));
}

base::Result<void> expect_tool_success(const ConsoleToolResult& result, const char* what) {
    if (!result.terminated && result.exit_code == 0) {
        return base::Result<void>::success();
    }
    std::string message = "command failed: ";
    message += what;
    message += " (exit ";
    message += std::to_string(result.exit_code);
    message += ") ";
    message += condense_console_output(result.output);
    return base::Result<void>::failure({base::ErrorCode::kIoFailure, std::move(message)});
}

std::string condense_console_output(const std::string_view text) {
    std::string excerpt;
    excerpt.reserve((std::min)(text.size(), kMaximumOutputExcerpt));
    bool pending_separator = false;
    for (const char character : text) {
        if (excerpt.size() >= kMaximumOutputExcerpt) {
            excerpt.append("...");
            break;
        }
        if (character == '\r') {
            continue;
        }
        if (character == '\n') {
            pending_separator = !excerpt.empty();
            continue;
        }
        if (pending_separator) {
            excerpt.append(" | ");
            pending_separator = false;
        }
        excerpt.push_back(character);
    }
    return excerpt;
}

bool output_references_token(const std::string_view output, const std::string_view token) {
    if (token.empty() || output.size() < token.size()) {
        return false;
    }
    const auto lower = [](const char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    };
    for (std::size_t start = 0; start + token.size() <= output.size(); ++start) {
        bool matched = true;
        for (std::size_t offset = 0; offset < token.size(); ++offset) {
            if (lower(output[start + offset]) != lower(token[offset])) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return true;
        }
    }
    return false;
}

} // namespace aegra::adapters::windows_pe::detail
