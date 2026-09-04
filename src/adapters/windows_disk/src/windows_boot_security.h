#pragma once

#include "aegra/adapters/windows_disk/windows_disk.h"

#include <filesystem>

namespace aegra::adapters::windows_disk::detail {

/// Queries Win32_EncryptableVolume protection state. Never requests key material.
[[nodiscard]] WindowsSecurityState
inspect_bitlocker_state(const std::filesystem::path& volume_guid_path) noexcept;

} // namespace aegra::adapters::windows_disk::detail
