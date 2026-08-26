#include "pe_image_internal.h"

#include "pe_boot_internal.h"
#include "pe_pending_internal.h"

#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_pe::detail {
namespace {

inline constexpr std::string_view kGlobalRootPrefix = R"(\\?\GLOBALROOT)";

[[nodiscard]] std::size_t find_token_case_insensitive(const std::string_view text,
                                                      const std::string_view token) {
    if (token.empty() || text.size() < token.size()) {
        return std::string_view::npos;
    }
    const auto lower = [](const char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    };
    for (std::size_t start = 0; start + token.size() <= text.size(); ++start) {
        bool matched = true;
        for (std::size_t offset = 0; offset < token.size(); ++offset) {
            if (lower(text[start + offset]) != lower(token[offset])) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return start;
        }
    }
    return std::string_view::npos;
}

/// Extracts the `\\?\GLOBALROOT\...\Recovery\WindowsRE` directory from
/// `reagentc /info` output. The GLOBALROOT token is locale-independent even
/// though the surrounding labels are localized.
[[nodiscard]] std::string extract_globalroot_directory(const std::string_view output) {
    const auto start = find_token_case_insensitive(output, kGlobalRootPrefix);
    if (start == std::string_view::npos) {
        return {};
    }
    auto end = output.find_first_of("\r\n", start);
    if (end == std::string_view::npos) {
        end = output.size();
    }
    std::string directory(output.substr(start, end - start));
    while (!directory.empty() && (directory.back() == ' ' || directory.back() == '\t')) {
        directory.pop_back();
    }
    return directory;
}

[[nodiscard]] base::Result<std::wstring> locate_winre_via_reagentc(
    ports::IProcessLauncher& launcher) {
    auto reagentc_path = resolve_system_tool_path("reagentc.exe");
    if (!reagentc_path) {
        return base::Result<std::wstring>::failure(reagentc_path.error());
    }
    auto result = run_console_tool(launcher, reagentc_path.value(), {"/info"});
    if (!result) {
        return base::Result<std::wstring>::failure(result.error());
    }
    const auto directory = extract_globalroot_directory(result.value().output);
    if (directory.empty()) {
        return base::Result<std::wstring>::failure(pe_store_error(
            base::ErrorCode::kNotFound, "reagentc did not report a windows re location"));
    }
    auto wide = utf8_to_wide(directory);
    if (!wide) {
        return base::Result<std::wstring>::failure(wide.error());
    }
    std::wstring candidate = std::move(wide).value();
    candidate += L"\\Winre.wim";
    if (!file_exists(candidate)) {
        return base::Result<std::wstring>::failure(pe_store_error(
            base::ErrorCode::kNotFound, "reagentc windows re location has no winre.wim"));
    }
    return base::Result<std::wstring>::success(std::move(candidate));
}

/// First fixed volume carrying `\Recovery\WindowsRE\Winre.wim`.
[[nodiscard]] std::wstring locate_winre_via_volume_scan() {
    wchar_t buffer[MAX_PATH]{};
    const HANDLE handle = FindFirstVolumeW(buffer, MAX_PATH);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::wstring found;
    do {
        const std::wstring volume(buffer);
        if (GetDriveTypeW(volume.c_str()) != DRIVE_FIXED) {
            continue;
        }
        std::wstring candidate = volume + L"Recovery\\WindowsRE\\Winre.wim";
        if (file_exists(candidate)) {
            found = std::move(candidate);
            break;
        }
    } while (FindNextVolumeW(handle, buffer, MAX_PATH));
    FindVolumeClose(handle);
    return found;
}

[[nodiscard]] base::Result<std::wstring> system_recovery_path(const std::wstring_view file_name) {
    wchar_t system_directory[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return base::Result<std::wstring>::failure(
            win32_error(GetLastError(), "resolve system directory"));
    }
    std::wstring path(system_directory, length);
    path += L"\\Recovery\\";
    path += file_name;
    return base::Result<std::wstring>::success(std::move(path));
}

[[nodiscard]] std::wstring parent_directory(const std::wstring& path) {
    const auto separator = path.find_last_of(L'\\');
    return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
}

class BcryptHash final {
  public:
    ~BcryptHash() {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }
    BcryptHash() = default;
    BcryptHash(const BcryptHash&) = delete;
    BcryptHash& operator=(const BcryptHash&) = delete;
    BcryptHash(BcryptHash&&) = delete;
    BcryptHash& operator=(BcryptHash&&) = delete;

    [[nodiscard]] base::Result<void> open() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            return base::Result<void>::failure(
                pe_store_error(base::ErrorCode::kInternal, "sha-256 provider is unavailable"));
        }
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<void> update(std::span<const std::byte> data) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) C API byte buffer.
        auto* bytes = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data()));
        if (BCryptHashData(hash_, bytes, static_cast<ULONG>(data.size()), 0) < 0) {
            return base::Result<void>::failure(
                pe_store_error(base::ErrorCode::kInternal, "sha-256 update failed"));
        }
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<std::string> finish_hex() {
        unsigned char digest[32]{};
        if (BCryptFinishHash(hash_, digest, sizeof(digest), 0) < 0) {
            return base::Result<std::string>::failure(
                pe_store_error(base::ErrorCode::kInternal, "sha-256 finish failed"));
        }
        static constexpr char kHexDigits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(sizeof(digest) * 2);
        for (const unsigned char value : digest) {
            hex.push_back(kHexDigits[value >> 4U]);
            hex.push_back(kHexDigits[value & 0x0FU]);
        }
        return base::Result<std::string>::success(std::move(hex));
    }

  private:
    BCRYPT_ALG_HANDLE algorithm_{nullptr};
    BCRYPT_HASH_HANDLE hash_{nullptr};
};

} // namespace

base::Result<PeImageSources> locate_pe_image_sources(ports::IProcessLauncher& launcher) {
    std::wstring winre;
    if (auto via_reagentc = locate_winre_via_reagentc(launcher); via_reagentc) {
        winre = std::move(via_reagentc).value();
    } else {
        winre = locate_winre_via_volume_scan();
    }
    if (winre.empty()) {
        auto fallback = system_recovery_path(L"Winre.wim");
        if (!fallback) {
            return base::Result<PeImageSources>::failure(fallback.error());
        }
        if (file_exists(fallback.value())) {
            winre = std::move(fallback).value();
        }
    }
    if (winre.empty()) {
        return base::Result<PeImageSources>::failure(pe_store_error(
            base::ErrorCode::kNotFound,
            "winre.wim was not found; enable windows re (reagentc /enable) and retry"));
    }
    auto sdi = system_recovery_path(L"boot.sdi");
    if (!sdi) {
        return base::Result<PeImageSources>::failure(sdi.error());
    }
    std::wstring boot_sdi = std::move(sdi).value();
    if (!file_exists(boot_sdi)) {
        boot_sdi = parent_directory(winre) + L"\\boot.sdi";
    }
    if (!file_exists(boot_sdi)) {
        return base::Result<PeImageSources>::failure(pe_store_error(
            base::ErrorCode::kNotFound, "boot.sdi was not found beside winre.wim or system32"));
    }
    PeImageSources sources;
    sources.winre_wim_path = std::move(winre);
    sources.boot_sdi_path = std::move(boot_sdi);
    return base::Result<PeImageSources>::success(std::move(sources));
}

base::Result<std::string> query_os_build_number() {
    wchar_t build[64]{};
    DWORD size = sizeof(build);
    const auto status =
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, build, &size);
    if (status != ERROR_SUCCESS) {
        return base::Result<std::string>::failure(
            win32_error(static_cast<DWORD>(status), "query os build number"));
    }
    std::string result;
    for (const wchar_t character : std::wstring_view(build)) {
        if (character < L'0' || character > L'9') {
            break;
        }
        result.push_back(static_cast<char>(character));
    }
    if (result.empty()) {
        return base::Result<std::string>::failure(
            pe_store_error(base::ErrorCode::kInternal, "os build number is unavailable"));
    }
    return base::Result<std::string>::success(std::move(result));
}

base::Result<std::string> sha256_file_hex(const std::wstring& path) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return base::Result<std::string>::failure(
            win32_error(GetLastError(), "open payload file for hashing"));
    }
    BcryptHash hash;
    if (auto opened = hash.open(); !opened) {
        return base::Result<std::string>::failure(opened.error());
    }
    std::vector<std::byte> buffer(64 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                      nullptr)) {
            return base::Result<std::string>::failure(
                win32_error(GetLastError(), "read payload file for hashing"));
        }
        if (read == 0) {
            break;
        }
        if (auto updated = hash.update({buffer.data(), read}); !updated) {
            return base::Result<std::string>::failure(updated.error());
        }
    }
    return hash.finish_hex();
}

base::Result<void> copy_file_writable(const std::wstring& source, const std::wstring& destination,
                                      const char* what) {
    if (!CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
        return base::Result<void>::failure(win32_error(GetLastError(), what));
    }
    if (!SetFileAttributesW(destination.c_str(), FILE_ATTRIBUTE_NORMAL)) {
        return base::Result<void>::failure(win32_error(GetLastError(), what));
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::windows_pe::detail
