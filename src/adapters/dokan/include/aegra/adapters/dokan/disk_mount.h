#pragma once

#include "aegra/base/result.h"
#include "aegra/format/manifest.h"
#include "aegra/ports/random_access.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::adapters::dokan {

struct MountSessionInfo {
    std::string session_id;
    std::uint32_t source_disk_number{0};
    std::string mount_point; // primary letter e.g. "E:"
    std::vector<std::string> drive_letters;
    std::uint32_t device_number{0xFFFFFFFF};
    std::uint64_t disk_size_bytes{0};
    std::string message_code; // empty on success
};

// reader must outlive the mount session until Unmount returns.
// preferred_drive_letter: empty or single letter "D".."Z"
// overlay_root: directory for Dokan mount point + overlay files (created if needed)
[[nodiscard]] base::Result<MountSessionInfo>
mount_whole_disk_readonly(ports::IRandomAccessReader& reader,
                          const format::Manifest& manifest,
                          std::uint32_t source_disk_number,
                          std::string_view preferred_drive_letter,
                          const std::filesystem::path& overlay_root,
                          std::string_view session_id);

[[nodiscard]] base::Result<void> unmount_session(std::string_view session_id);
[[nodiscard]] base::Result<void> unmount_all_sessions();
[[nodiscard]] std::vector<MountSessionInfo> list_sessions();
[[nodiscard]] bool is_dokan_available() noexcept;

} // namespace aegra::adapters::dokan
