#pragma once

#include "aegra/base/result.h"

#include <Windows.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace aegra::adapters::windows_filesystem::detail {

/// Enables the privileges required to read content and full security descriptors.
[[nodiscard]] base::Result<void> enable_file_backup_privileges();

/// Enables the privileges required to restore content and full security descriptors.
[[nodiscard]] base::Result<void> enable_file_restore_privileges();

/// Reads Owner/Group/DACL/SACL as a self-relative SECURITY_DESCRIPTOR.
/// On failure after privileges: message is file_source.security_descriptor_unreadable.
[[nodiscard]] base::Result<std::vector<std::byte>>
read_self_relative_security_descriptor(const std::wstring& absolute_path);

/// Applies a self-relative SECURITY_DESCRIPTOR (Owner/Group/DACL/SACL) to an existing path.
[[nodiscard]] base::Result<void>
write_self_relative_security_descriptor(const std::wstring& absolute_path,
                                        std::span<const std::byte> self_relative_sd);

/// Applies SD to an open handle (staged file before publish).
[[nodiscard]] base::Result<void>
write_self_relative_security_descriptor(HANDLE handle,
                                        std::span<const std::byte> self_relative_sd);

} // namespace aegra::adapters::windows_filesystem::detail
