#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include <windows.h>

namespace aegra::adapters::dokan::detail {

struct VhdAttachResult {
    HANDLE vhd_handle{INVALID_HANDLE_VALUE};
    std::wstring physical_path;
    int windows_disk_number{-1};
    std::vector<std::string> drive_letters; // data volumes only, e.g. "Z:"
    std::uint64_t total_data_size{0};
    std::string error;
};

// Attach a VHDX read-only, online the disk, assign letters only to data partitions.
[[nodiscard]] bool attach_vhdx_readonly(const std::wstring& vhdx_path,
                                        const std::set<std::uint32_t>& data_partitions,
                                        const std::string& preferred_drive_letter,
                                        VhdAttachResult& out);

[[nodiscard]] bool detach_vhd_handle(HANDLE vhd_handle);

[[nodiscard]] bool drive_letter_exists(const std::string& letter);

[[nodiscard]] std::string find_free_drive_letter();

[[nodiscard]] std::string normalize_drive_letter(const std::string& in);

} // namespace aegra::adapters::dokan::detail
