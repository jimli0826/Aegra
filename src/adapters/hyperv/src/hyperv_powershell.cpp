#include "hyperv_powershell.h"

#include <utility>
#include <vector>

namespace aegra::adapters::hyperv::detail {
namespace {

/// Base64 of the UTF-16LE encoding of an ASCII/UTF-8 script. Scripts are built
/// from ASCII literals plus single-quoted UTF-8 paths; PowerShell decodes the
/// UTF-16LE stream, so non-ASCII UTF-8 bytes must first widen properly.
[[nodiscard]] std::string encode_powershell_command(const std::string_view script) {
    // Widen UTF-8 -> UTF-16 (manual decoder keeps this adapter free of Windows
    // headers in this translation unit).
    std::u16string wide;
    wide.reserve(script.size());
    for (std::size_t index = 0; index < script.size();) {
        const auto lead = static_cast<unsigned char>(script[index]);
        char32_t code_point = 0;
        std::size_t length = 1;
        if (lead < 0x80U) {
            code_point = lead;
        } else if ((lead & 0xE0U) == 0xC0U && index + 1 < script.size()) {
            code_point = static_cast<char32_t>(lead & 0x1FU) << 6 |
                         (static_cast<unsigned char>(script[index + 1]) & 0x3FU);
            length = 2;
        } else if ((lead & 0xF0U) == 0xE0U && index + 2 < script.size()) {
            code_point =
                static_cast<char32_t>(lead & 0x0FU) << 12 |
                (static_cast<char32_t>(static_cast<unsigned char>(script[index + 1]) & 0x3FU)
                 << 6) |
                (static_cast<unsigned char>(script[index + 2]) & 0x3FU);
            length = 3;
        } else if ((lead & 0xF8U) == 0xF0U && index + 3 < script.size()) {
            code_point =
                static_cast<char32_t>(lead & 0x07U) << 18 |
                (static_cast<char32_t>(static_cast<unsigned char>(script[index + 1]) & 0x3FU)
                 << 12) |
                (static_cast<char32_t>(static_cast<unsigned char>(script[index + 2]) & 0x3FU)
                 << 6) |
                (static_cast<unsigned char>(script[index + 3]) & 0x3FU);
            length = 4;
        } else {
            code_point = 0xFFFDU;
        }
        index += length;
        if (code_point >= 0x10000U) {
            const auto value = code_point - 0x10000U;
            wide.push_back(static_cast<char16_t>(0xD800U + (value >> 10)));
            wide.push_back(static_cast<char16_t>(0xDC00U + (value & 0x3FFU)));
        } else {
            wide.push_back(static_cast<char16_t>(code_point));
        }
    }
    std::vector<unsigned char> bytes;
    bytes.reserve(wide.size() * 2);
    for (const char16_t unit : wide) {
        bytes.push_back(static_cast<unsigned char>(unit & 0xFFU));
        bytes.push_back(static_cast<unsigned char>(unit >> 8));
    }
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const unsigned first = bytes[index];
        const unsigned second = index + 1 < bytes.size() ? bytes[index + 1] : 0U;
        const unsigned third = index + 2 < bytes.size() ? bytes[index + 2] : 0U;
        encoded.push_back(kAlphabet[first >> 2]);
        encoded.push_back(kAlphabet[((first & 0x3U) << 4) | (second >> 4)]);
        encoded.push_back(
            index + 1 < bytes.size() ? kAlphabet[((second & 0xFU) << 2) | (third >> 6)] : '=');
        encoded.push_back(index + 2 < bytes.size() ? kAlphabet[third & 0x3FU] : '=');
    }
    return encoded;
}

} // namespace

PowerShellRunner::PowerShellRunner(ports::IProcessLauncher& launcher, std::string powershell_path)
    : launcher_(&launcher), powershell_path_(std::move(powershell_path)) {}

base::Result<PowerShellResult> PowerShellRunner::run(const std::string_view script,
                                                     const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<PowerShellResult>::failure(
            {base::ErrorCode::kCancelled, "bootcheck.cancelled"});
    }
    // Strict mode so any cmdlet failure becomes a terminating error -> exit 1.
    std::string strict_script = "$ErrorActionPreference = 'Stop'; ";
    strict_script.append(script);
    ports::ProcessLaunchRequest request;
    request.executable_path = powershell_path_;
    request.arguments = {"-NoProfile", "-NonInteractive", "-ExecutionPolicy",
                         "Bypass",     "-EncodedCommand", encode_powershell_command(strict_script)};
    request.capture_output = true;
    auto launched = launcher_->launch(request);
    if (!launched) {
        return base::Result<PowerShellResult>::failure(launched.error());
    }
    auto waited = launcher_->wait(launched.value().pid, cancellation);
    if (!waited && waited.error().code == base::ErrorCode::kCancelled) {
        (void)launcher_->terminate(launched.value().pid);
        (void)launcher_->wait(launched.value().pid, base::CancellationToken{});
        return base::Result<PowerShellResult>::failure(
            {base::ErrorCode::kCancelled, "bootcheck.cancelled"});
    }
    if (!waited) {
        return base::Result<PowerShellResult>::failure(waited.error());
    }
    PowerShellResult result;
    result.exit_code = waited.value().exit_code;
    result.output = std::move(waited.value().output);
    return base::Result<PowerShellResult>::success(std::move(result));
}

std::string quote_powershell(const std::string_view value) {
    std::string quoted = "'";
    for (const char character : value) {
        quoted.push_back(character);
        if (character == '\'') {
            quoted.push_back('\'');
        }
    }
    quoted.push_back('\'');
    return quoted;
}

} // namespace aegra::adapters::hyperv::detail
