#pragma once

#include <string>

namespace aegra::adapters::virtualbox {

/// Resolves the trusted absolute VBoxManage.exe path from the machine-wide
/// VirtualBox installation (registry InstallDir, then the default Program Files
/// location). Returns an empty string when VirtualBox is not installed. The
/// returned path still goes through the provider's signature and version
/// checks before any command is launched.
[[nodiscard]] std::string discover_vbox_manage_path();

} // namespace aegra::adapters::virtualbox
