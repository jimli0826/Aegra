#include "virt_disk_attach.h"

#include <initguid.h>
#include <shlobj.h>
#include <virtdisk.h>
#include <winioctl.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace aegra::adapters::dokan::detail {
namespace {

bool is_letter_free(char letter) {
    const char c =
        static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    if (c < 'A' || c > 'Z') {
        return false;
    }
    const DWORD mask = GetLogicalDrives();
    return (mask & (1u << (c - 'A'))) == 0;
}

int parse_physical_drive_number(const std::wstring& physical_path) {
    const wchar_t* p = physical_path.c_str();
    const wchar_t* found = wcsstr(p, L"PhysicalDrive");
    if (!found) {
        return -1;
    }
    found += wcslen(L"PhysicalDrive");
    if (*found == L'\0') {
        return -1;
    }
    return _wtoi(found);
}

bool online_disk(int disk_number) {
    const std::wstring path =
        L"\\\\.\\PhysicalDrive" + std::to_wstring(disk_number);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return false;
        }
    }

    SET_DISK_ATTRIBUTES attrs{};
    attrs.Version = sizeof(attrs);
    attrs.Attributes = DISK_ATTRIBUTE_READ_ONLY;
    attrs.AttributesMask = DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY;
    DWORD br = 0;
    DeviceIoControl(h, IOCTL_DISK_SET_DISK_ATTRIBUTES, &attrs, sizeof(attrs), nullptr,
                    0, &br, nullptr);
    DeviceIoControl(h, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &br,
                    nullptr);
    CloseHandle(h);
    return true;
}

struct VolumeInfo {
    std::wstring volume_name;
    std::uint32_t partition_number{0};
    std::uint64_t size{0};
    std::string letter;
};

bool get_volume_letter(const std::wstring& volume_name, std::string& out_letter) {
    out_letter.clear();
    DWORD needed = 0;
    GetVolumePathNamesForVolumeNameW(volume_name.c_str(), nullptr, 0, &needed);
    if (needed == 0) {
        return false;
    }
    std::vector<wchar_t> buf(needed);
    if (!GetVolumePathNamesForVolumeNameW(volume_name.c_str(), buf.data(), needed,
                                          &needed)) {
        return false;
    }
    if (buf[0] == L'\0') {
        return true;
    }
    if (iswalpha(buf[0]) && buf[1] == L':') {
        const char c = static_cast<char>(towupper(buf[0]));
        out_letter = std::string(1, c) + ":";
    }
    return true;
}

bool get_volume_disk_extent(const std::wstring& volume_name, int& disk_number,
                            std::uint64_t& extent_length) {
    disk_number = -1;
    extent_length = 0;

    std::wstring open_path = volume_name;
    if (!open_path.empty() && open_path.back() == L'\\') {
        open_path.pop_back();
    }

    HANDLE h = CreateFileW(open_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    BYTE stack_buf[sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 4] = {};
    DWORD br = 0;
    const BOOL ok =
        DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, stack_buf,
                        sizeof(stack_buf), &br, nullptr);
    if (!ok) {
        CloseHandle(h);
        return false;
    }
    auto* extents = reinterpret_cast<VOLUME_DISK_EXTENTS*>(stack_buf);
    if (extents->NumberOfDiskExtents < 1) {
        CloseHandle(h);
        return false;
    }
    disk_number = static_cast<int>(extents->Extents[0].DiskNumber);
    extent_length =
        static_cast<std::uint64_t>(extents->Extents[0].ExtentLength.QuadPart);
    CloseHandle(h);
    return true;
}

bool get_volume_partition_number(const std::wstring& volume_name,
                                 std::uint32_t& part_num, std::uint64_t& size) {
    part_num = 0;
    size = 0;
    std::wstring open_path = volume_name;
    if (!open_path.empty() && open_path.back() == L'\\') {
        open_path.pop_back();
    }

    HANDLE h = CreateFileW(open_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD br = 0;
    PARTITION_INFORMATION_EX part_info{};
    if (DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0, &part_info,
                        sizeof(part_info), &br, nullptr)) {
        part_num = static_cast<std::uint32_t>(part_info.PartitionNumber);
        size = static_cast<std::uint64_t>(part_info.PartitionLength.QuadPart);
        CloseHandle(h);
        return part_num > 0;
    }

    GET_LENGTH_INFORMATION len_info{};
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len_info,
                        sizeof(len_info), &br, nullptr)) {
        size = static_cast<std::uint64_t>(len_info.Length.QuadPart);
    }
    CloseHandle(h);
    return false;
}

std::vector<VolumeInfo> collect_volumes_on_disk(int windows_disk_number) {
    std::vector<VolumeInfo> result;
    wchar_t vol_name[MAX_PATH] = {};
    HANDLE h_find = FindFirstVolumeW(vol_name, MAX_PATH);
    if (h_find == INVALID_HANDLE_VALUE) {
        return result;
    }

    do {
        int disk_num = -1;
        std::uint64_t len = 0;
        if (!get_volume_disk_extent(vol_name, disk_num, len)) {
            continue;
        }
        if (disk_num != windows_disk_number) {
            continue;
        }

        VolumeInfo vi;
        vi.volume_name = vol_name;
        get_volume_letter(vol_name, vi.letter);
        std::uint32_t part_num = 0;
        std::uint64_t size = 0;
        if (get_volume_partition_number(vol_name, part_num, size)) {
            vi.partition_number = part_num;
            vi.size = size;
        } else {
            vi.size = len;
        }
        result.push_back(std::move(vi));
    } while (FindNextVolumeW(h_find, vol_name, MAX_PATH));

    FindVolumeClose(h_find);
    std::sort(result.begin(), result.end(),
              [](const VolumeInfo& a, const VolumeInfo& b) {
                  return a.partition_number < b.partition_number;
              });
    return result;
}

bool remove_volume_letter(const std::wstring& /*volume_name*/,
                          const std::string& letter) {
    if (letter.empty()) {
        return true;
    }
    const std::wstring mount =
        std::wstring(1, static_cast<wchar_t>(letter[0])) + L":\\";
    return DeleteVolumeMountPointW(mount.c_str()) != FALSE;
}

bool assign_volume_letter(const std::wstring& volume_name, const std::string& letter) {
    const std::string L = normalize_drive_letter(letter);
    if (L.empty()) {
        return false;
    }
    const std::wstring mount = std::wstring(1, static_cast<wchar_t>(L[0])) + L":\\";
    std::wstring vol = volume_name;
    if (vol.empty() || vol.back() != L'\\') {
        vol.push_back(L'\\');
    }
    return SetVolumeMountPointW(mount.c_str(), vol.c_str()) != FALSE;
}

void notify_shell_drive(const std::string& letter, bool added) {
    const std::string L = normalize_drive_letter(letter);
    if (L.empty()) {
        return;
    }
    const std::wstring path = std::wstring(1, static_cast<wchar_t>(L[0])) + L":\\";
    SHChangeNotify(added ? SHCNE_DRIVEADD : SHCNE_DRIVEREMOVED,
                   SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr);
}

bool open_vhdx_readonly(const std::wstring& vhdx_path, HANDLE& handle,
                        std::string& error) {
    VIRTUAL_STORAGE_TYPE storage_type{};
    storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storage_type.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    OPEN_VIRTUAL_DISK_PARAMETERS open_params{};
    open_params.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    open_params.Version2.GetInfoOnly = FALSE;
    open_params.Version2.ReadOnly = TRUE;
    open_params.Version2.ResiliencyGuid = {};

    handle = INVALID_HANDLE_VALUE;
    DWORD err = OpenVirtualDisk(
        &storage_type, vhdx_path.c_str(),
        VIRTUAL_DISK_ACCESS_ATTACH_RO | VIRTUAL_DISK_ACCESS_GET_INFO,
        OPEN_VIRTUAL_DISK_FLAG_NONE, &open_params, &handle);
    if (err != ERROR_SUCCESS) {
        OPEN_VIRTUAL_DISK_PARAMETERS open1{};
        open1.Version = OPEN_VIRTUAL_DISK_VERSION_1;
        open1.Version1.RWDepth = 0;
        err = OpenVirtualDisk(
            &storage_type, vhdx_path.c_str(),
            VIRTUAL_DISK_ACCESS_ATTACH_RO | VIRTUAL_DISK_ACCESS_GET_INFO,
            OPEN_VIRTUAL_DISK_FLAG_NONE, &open1, &handle);
    }
    if (err != ERROR_SUCCESS || handle == INVALID_HANDLE_VALUE) {
        error = "OpenVirtualDisk failed";
        return false;
    }
    return true;
}

bool attach_handle_readonly(HANDLE handle, std::string& error) {
    ATTACH_VIRTUAL_DISK_PARAMETERS attach_params{};
    attach_params.Version = ATTACH_VIRTUAL_DISK_VERSION_1;

    DWORD err = AttachVirtualDisk(
        handle, nullptr,
        ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY | ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER,
        0, &attach_params, nullptr);
    if (err != ERROR_SUCCESS) {
        err = AttachVirtualDisk(handle, nullptr, ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY, 0,
                                &attach_params, nullptr);
    }
    if (err != ERROR_SUCCESS) {
        error = "AttachVirtualDisk failed";
        return false;
    }
    return true;
}

bool resolve_physical_path(HANDLE handle, VhdAttachResult& out) {
    wchar_t phys_buf[MAX_PATH] = {};
    ULONG phys_size = sizeof(phys_buf);
    DWORD err = GetVirtualDiskPhysicalPath(handle, &phys_size, phys_buf);
    if (err == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<wchar_t> big(phys_size / sizeof(wchar_t) + 2, 0);
        err = GetVirtualDiskPhysicalPath(handle, &phys_size, big.data());
        if (err == ERROR_SUCCESS) {
            out.physical_path.assign(big.data());
        }
    } else if (err == ERROR_SUCCESS) {
        out.physical_path = phys_buf;
    }
    if (out.physical_path.empty()) {
        out.error = "GetVirtualDiskPhysicalPath failed";
        return false;
    }
    out.windows_disk_number = parse_physical_drive_number(out.physical_path);
    if (out.windows_disk_number < 0) {
        out.error = "Could not parse physical drive number";
        return false;
    }
    return true;
}

void assign_data_volume_letters(std::vector<VolumeInfo>& data_vols,
                                const std::string& preferred, VhdAttachResult& out) {
    for (std::size_t i = 0; i < data_vols.size(); ++i) {
        auto& v = data_vols[i];
        std::string want;
        if (i == 0 && !preferred.empty() &&
            (is_letter_free(preferred[0]) || v.letter == preferred)) {
            want = preferred;
        } else if (!v.letter.empty()) {
            want = v.letter;
        } else {
            want = find_free_drive_letter();
        }

        if (want.empty()) {
            continue;
        }

        if (v.letter == want) {
            out.drive_letters.push_back(want);
            out.total_data_size += v.size;
            notify_shell_drive(want, true);
            continue;
        }

        if (!v.letter.empty() && v.letter != want) {
            remove_volume_letter(v.volume_name, v.letter);
        }

        if (assign_volume_letter(v.volume_name, want)) {
            v.letter = want;
            out.drive_letters.push_back(want);
            out.total_data_size += v.size;
            notify_shell_drive(want, true);
        } else {
            const std::string alt = find_free_drive_letter();
            if (!alt.empty() && assign_volume_letter(v.volume_name, alt)) {
                out.drive_letters.push_back(alt);
                out.total_data_size += v.size;
                notify_shell_drive(alt, true);
            }
        }
    }
}

std::vector<VolumeInfo>
select_data_volumes(const std::vector<VolumeInfo>& volumes,
                    const std::set<std::uint32_t>& data_partitions) {
    std::vector<VolumeInfo> data_vols;
    for (const auto& v : volumes) {
        const bool is_data =
            v.partition_number > 0 && data_partitions.count(v.partition_number) > 0;
        if (!is_data) {
            if (!v.letter.empty()) {
                remove_volume_letter(v.volume_name, v.letter);
            }
            continue;
        }
        data_vols.push_back(v);
    }

    if (data_vols.empty()) {
        for (const auto& v : volumes) {
            if (v.size >= 256ULL * 1024 * 1024) {
                data_vols.push_back(v);
            } else if (!v.letter.empty()) {
                remove_volume_letter(v.volume_name, v.letter);
            }
        }
    }

    std::sort(data_vols.begin(), data_vols.end(),
              [](const VolumeInfo& a, const VolumeInfo& b) {
                  return a.partition_number < b.partition_number;
              });
    return data_vols;
}

} // namespace

std::string normalize_drive_letter(const std::string& in) {
    if (in.empty()) {
        return {};
    }
    const char c =
        static_cast<char>(std::toupper(static_cast<unsigned char>(in[0])));
    if (c < 'A' || c > 'Z') {
        return {};
    }
    return std::string(1, c) + ":";
}

bool drive_letter_exists(const std::string& letter) {
    const std::string L = normalize_drive_letter(letter);
    if (L.empty()) {
        return false;
    }
    const std::wstring mp = std::wstring(1, static_cast<wchar_t>(L[0])) + L":";
    wchar_t target[MAX_PATH] = {};
    return QueryDosDeviceW(mp.c_str(), target, MAX_PATH) != 0;
}

std::string find_free_drive_letter() {
    for (char c = 'Z'; c >= 'D'; --c) {
        if (is_letter_free(c)) {
            return std::string(1, c) + ":";
        }
    }
    return {};
}

bool detach_vhd_handle(HANDLE vhd_handle) {
    if (vhd_handle == nullptr || vhd_handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD err = DetachVirtualDisk(vhd_handle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    CloseHandle(vhd_handle);
    return err == ERROR_SUCCESS;
}

bool attach_vhdx_readonly(const std::wstring& vhdx_path,
                          const std::set<std::uint32_t>& data_partitions,
                          const std::string& preferred_drive_letter,
                          VhdAttachResult& out) {
    out = VhdAttachResult{};

    if (vhdx_path.empty()) {
        out.error = "empty VHDX path";
        return false;
    }
    if (data_partitions.empty()) {
        out.error = "no data partitions specified";
        return false;
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    if (!open_vhdx_readonly(vhdx_path, handle, out.error)) {
        return false;
    }
    if (!attach_handle_readonly(handle, out.error)) {
        CloseHandle(handle);
        return false;
    }

    out.vhd_handle = handle;
    if (!resolve_physical_path(handle, out)) {
        (void)detach_vhd_handle(handle);
        out.vhd_handle = INVALID_HANDLE_VALUE;
        return false;
    }

    (void)online_disk(out.windows_disk_number);

    std::vector<VolumeInfo> volumes;
    for (int i = 0; i < 40; ++i) {
        volumes = collect_volumes_on_disk(out.windows_disk_number);
        if (!volumes.empty()) {
            break;
        }
        Sleep(250);
    }

    auto data_vols = select_data_volumes(volumes, data_partitions);
    if (data_vols.empty()) {
        out.error = "No data volumes found after VHD attach";
        (void)detach_vhd_handle(handle);
        out.vhd_handle = INVALID_HANDLE_VALUE;
        return false;
    }

    const std::string preferred = normalize_drive_letter(preferred_drive_letter);
    assign_data_volume_letters(data_vols, preferred, out);

    if (out.drive_letters.empty()) {
        out.error = "Failed to assign any drive letter to data volumes";
        (void)detach_vhd_handle(handle);
        out.vhd_handle = INVALID_HANDLE_VALUE;
        return false;
    }
    return true;
}

} // namespace aegra::adapters::dokan::detail
