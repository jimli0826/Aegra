#pragma once

#include "aegra/base/result.h"
#include "aegra/ports/process_launcher.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::adapters::windows_pe::detail {

/// Fixed product GUIDs for the recovery boot entry and its ramdisk device options.
/// They are created explicitly (`bcdedit /create {guid}`), so success/failure and
/// identity never depend on parsing localized bcdedit text. Re-arming deletes and
/// recreates them; uninstall/disarm removes them.
inline constexpr std::string_view kBootEntryGuid = "{a7c3e9f2-58d1-4b6a-9e04-c2f7b8315d6a}";
inline constexpr std::string_view kDeviceOptionsGuid = "{a7c3e9f2-58d1-4b6a-9e04-c2f7b8315d6b}";

struct ConsoleToolResult final {
    std::uint32_t exit_code{0};
    bool terminated{false};
    /// Combined console output (diagnostics only; may be localized).
    std::string output;
};

/// `<system directory>\<tool_name>` as UTF-8 (e.g. "bcdedit.exe", "dism.exe").
[[nodiscard]] base::Result<std::string> resolve_system_tool_path(std::string_view tool_name);

/// kUnauthorized unless the process token is elevated (BCD and DISM require it).
[[nodiscard]] base::Result<void> require_elevated();

/// Runs a console tool to completion with captured output. The wait is
/// deliberately uncancellable so a BCD or DISM operation is never abandoned in
/// an unknown state; callers honor cancellation between commands.
[[nodiscard]] base::Result<ConsoleToolResult> run_console_tool(ports::IProcessLauncher& launcher,
                                                               const std::string& tool_path_utf8,
                                                               std::vector<std::string> arguments);

/// Fails with a bounded output excerpt when the command did not exit with 0.
[[nodiscard]] base::Result<void> expect_tool_success(const ConsoleToolResult& result,
                                                     const char* what);

/// Case-insensitive ASCII search (used for brace GUIDs and `\\?\GLOBALROOT`
/// tokens, which are locale-independent unlike the surrounding field labels).
[[nodiscard]] bool output_references_token(std::string_view output, std::string_view token);

/// One bounded, single-line excerpt of console output for diagnostics/errors.
[[nodiscard]] std::string condense_console_output(std::string_view text);

} // namespace aegra::adapters::windows_pe::detail
