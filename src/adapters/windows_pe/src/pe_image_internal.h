#pragma once

#include "aegra/base/result.h"
#include "aegra/ports/process_launcher.h"

#include <string>
#include <string_view>

namespace aegra::adapters::windows_pe::detail {

inline constexpr std::wstring_view kImageDirRelative = L"pe\\image";
/// Former source-WIM cache; deleted on sight so a leftover never shadows Recovery.
inline constexpr std::wstring_view kLegacyBaseWimFileName = L"winre_base.wim";
inline constexpr std::wstring_view kBootWimFileName = L"boot.wim";
inline constexpr std::wstring_view kBootSdiFileName = L"boot.sdi";
inline constexpr std::wstring_view kBuildIdFileName = L"build_id.json";
inline constexpr std::wstring_view kMountDirName = L"mount";
/// Payload directory inside the mounted WIM.
inline constexpr std::wstring_view kPayloadDirRelative = L"Windows\\System32\\Aegra";
inline constexpr std::wstring_view kWinpeshlRelative = L"Windows\\System32\\winpeshl.ini";

/// Located WinRE source files on the online system (wide, absolute).
struct PeImageSources final {
    std::wstring winre_wim_path;
    std::wstring boot_sdi_path;
};

/// Locates `Winre.wim` and `boot.sdi` by priority: `reagentc /info` GLOBALROOT
/// token → fixed-volume scan of `\Recovery\WindowsRE\` → `<system32>\Recovery`.
[[nodiscard]] base::Result<PeImageSources> locate_pe_image_sources(
    ports::IProcessLauncher& launcher);

/// Host OS build number from the registry (`CurrentBuildNumber`).
[[nodiscard]] base::Result<std::string> query_os_build_number();

/// Lowercase-hex SHA-256 of a file's content via BCrypt (CNG). Windows platform
/// hashing keeps this adapter free of the crypto adapter's libsodium dependency.
[[nodiscard]] base::Result<std::string> sha256_file_hex(const std::wstring& path);

/// Copies a file with Win32 and clears read-only on the destination.
[[nodiscard]] base::Result<void> copy_file_writable(const std::wstring& source,
                                                    const std::wstring& destination,
                                                    const char* what);

} // namespace aegra::adapters::windows_pe::detail
