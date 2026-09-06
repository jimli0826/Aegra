#include "virt_disk_attach.h"

#include <initguid.h>
#include <shlobj.h>
#include <virtdisk.h>
#include <winioctl.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::adapters::dokan::detail {
namespace {

[[nodiscard]] std::string win32_failure(const std::string_view operation,
                                        const DWORD error) {
    std::string message(operation);
    message.append(" failed (win32=");
    message.append(std::to_string(error));
    message.push_back(')');
    return message;
}

[[nodiscard]] std::string path_diagnostic(const std::wstring_view path) {
    if (path.empty()) {
        return "<empty>";
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
                                             static_cast<int>(path.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (required <= 0) {
        return "<utf8-conversion-failed>";
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
                            static_cast<int>(path.size()), result.data(), required, nullptr,
                            nullptr) <= 0) {
        return "<utf8-conversion-failed>";
    }
    return result;
}

bool is_letter_free(char letter) {
    const char c =
        static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    if (c < 'A' || c > 'Z') {
        return false;
    }
    const DWORD mask = GetLogicalDrives();
    return (mask & (1u << (c - 'A'))) == 0;
}

int parse_physical_drive_number(const std::wstring_view physical_path) {
    constexpr std::wstring_view kMarker = L"physicaldrive";
    std::size_t digits_begin = std::wstring_view::npos;
    for (std::size_t offset = 0; offset + kMarker.size() <= physical_path.size(); ++offset) {
        bool matches = true;
        for (std::size_t index = 0; index < kMarker.size(); ++index) {
            wchar_t character = physical_path[offset + index];
            if (character >= L'A' && character <= L'Z') {
                character = static_cast<wchar_t>(character - L'A' + L'a');
            }
            if (character != kMarker[index]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            digits_begin = offset + kMarker.size();
            break;
        }
    }
    if (digits_begin == std::wstring_view::npos || digits_begin == physical_path.size()) {
        return -1;
    }

    unsigned int number = 0;
    for (std::size_t index = digits_begin; index < physical_path.size(); ++index) {
        const wchar_t character = physical_path[index];
        if (character < L'0' || character > L'9') {
            return -1;
        }
        const unsigned int digit = static_cast<unsigned int>(character - L'0');
        if (number > (static_cast<unsigned int>(std::numeric_limits<int>::max()) - digit) / 10U) {
            return -1;
        }
        number = number * 10U + digit;
    }
    return static_cast<int>(number);
}

// Diagnostic record of the attempt to online the attached disk; a silent
// failure here previously left "no volumes" errors without a root cause.
struct OnlineDiskOutcome {
    bool opened{false};
    bool writable{false};
    DWORD open_error{0};
    bool set_attributes_ok{false};
    DWORD set_attributes_error{0};
};

OnlineDiskOutcome online_disk(int disk_number) {
    OnlineDiskOutcome outcome;
    const std::wstring path =
        L"\\\\.\\PhysicalDrive" + std::to_wstring(disk_number);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        outcome.writable = true;
    } else {
        outcome.open_error = GetLastError();
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            outcome.open_error = GetLastError();
            return outcome;
        }
    }
    outcome.opened = true;

    DWORD br = 0;

    // Step 1 (required): clear the OFFLINE attribute only — exactly what Disk
    // Manager's "Online" does. Bundling the READ_ONLY change into the same call
    // makes Windows more likely to reject it with ERROR_WRITE_PROTECT, and right
    // after attach the disk may briefly refuse attribute changes at all, so the
    // caller retries this in the volume-wait loop.
    SET_DISK_ATTRIBUTES clear_offline{};
    clear_offline.Version = sizeof(clear_offline);
    clear_offline.Attributes = 0;
    clear_offline.AttributesMask = DISK_ATTRIBUTE_OFFLINE;
    if (DeviceIoControl(h, IOCTL_DISK_SET_DISK_ATTRIBUTES, &clear_offline,
                        sizeof(clear_offline), nullptr, 0, &br, nullptr)) {
        outcome.set_attributes_ok = true;
    } else {
        outcome.set_attributes_error = GetLastError();
    }

    // Step 2 (best-effort): mark the disk read-only for the user. Not fatal if it
    // fails — any stray write still lands in the CoW overlay, never the archive.
    if (outcome.set_attributes_ok) {
        SET_DISK_ATTRIBUTES set_read_only{};
        set_read_only.Version = sizeof(set_read_only);
        set_read_only.Attributes = DISK_ATTRIBUTE_READ_ONLY;
        set_read_only.AttributesMask = DISK_ATTRIBUTE_READ_ONLY;
        DeviceIoControl(h, IOCTL_DISK_SET_DISK_ATTRIBUTES, &set_read_only,
                        sizeof(set_read_only), nullptr, 0, &br, nullptr);
    }

    DeviceIoControl(h, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &br,
                    nullptr);
    CloseHandle(h);
    return outcome;
}

// Post-mortem state of the attached disk, queried when no volumes surfaced.
// An OFFLINE disk with a readable partition table is the signature/GUID
// collision case (the mounted image clones a disk that is already online,
// e.g. the host was restored from this very image).
struct DiskStateDiagnostic {
    bool attributes_ok{false};
    bool offline{false};
    bool disk_read_only{false};
    DWORD attributes_error{0};
    int partition_count{-1};
    DWORD layout_error{0};
};

DiskStateDiagnostic query_disk_state(int disk_number) {
    DiskStateDiagnostic state;
    const std::wstring path =
        L"\\\\.\\PhysicalDrive" + std::to_wstring(disk_number);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) {
        state.attributes_error = GetLastError();
        state.layout_error = state.attributes_error;
        return state;
    }

    GET_DISK_ATTRIBUTES attrs{};
    DWORD br = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0, &attrs,
                        sizeof(attrs), &br, nullptr)) {
        state.attributes_ok = true;
        state.offline = (attrs.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0;
        state.disk_read_only = (attrs.Attributes & DISK_ATTRIBUTE_READ_ONLY) != 0;
    } else {
        state.attributes_error = GetLastError();
    }

    std::vector<std::byte> layout_buf(sizeof(DRIVE_LAYOUT_INFORMATION_EX) +
                                      sizeof(PARTITION_INFORMATION_EX) * 63);
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, layout_buf.data(),
                        static_cast<DWORD>(layout_buf.size()), &br, nullptr)) {
        const auto* layout =
            reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(layout_buf.data());
        state.partition_count = static_cast<int>(layout->PartitionCount);
    } else {
        state.layout_error = GetLastError();
    }
    CloseHandle(h);
    return state;
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

// Media writability is chosen per attach attempt. Read-only media is the safest
// presentation: the volumes mount read-only and nothing can ever write. But a
// host that surfaces the new disk OFFLINE (e.g. Server 2019 under SAN policy
// OfflineShared) can never online a read-only disk — SET_DISK_ATTRIBUTES fails
// with ERROR_WRITE_PROTECT (win32=19) — so the caller falls back to writable
// media: the offline bit can then be cleared and the disk is marked READ_ONLY
// before its volumes mount; any stray write is absorbed by the CoW overlay and
// discarded at unmount — the archive is never modified.
bool open_vhdx(const std::wstring& vhdx_path, const bool writable, HANDLE& handle,
               std::string& error) {
    VIRTUAL_STORAGE_TYPE storage_type{};
    storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storage_type.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    const auto access = static_cast<VIRTUAL_DISK_ACCESS_MASK>(
        (writable ? VIRTUAL_DISK_ACCESS_ATTACH_RW : VIRTUAL_DISK_ACCESS_ATTACH_RO) |
        VIRTUAL_DISK_ACCESS_GET_INFO);

    OPEN_VIRTUAL_DISK_PARAMETERS open_params{};
    open_params.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    open_params.Version2.GetInfoOnly = FALSE;
    open_params.Version2.ReadOnly = writable ? FALSE : TRUE;
    open_params.Version2.ResiliencyGuid = {};

    handle = INVALID_HANDLE_VALUE;
    DWORD err = OpenVirtualDisk(&storage_type, vhdx_path.c_str(), access,
                                OPEN_VIRTUAL_DISK_FLAG_NONE, &open_params, &handle);
    if (err != ERROR_SUCCESS) {
        OPEN_VIRTUAL_DISK_PARAMETERS open1{};
        open1.Version = OPEN_VIRTUAL_DISK_VERSION_1;
        open1.Version1.RWDepth = writable ? OPEN_VIRTUAL_DISK_RW_DEPTH_DEFAULT : 0;
        err = OpenVirtualDisk(&storage_type, vhdx_path.c_str(), access,
                              OPEN_VIRTUAL_DISK_FLAG_NONE, &open1, &handle);
    }
    if (err != ERROR_SUCCESS || handle == INVALID_HANDLE_VALUE) {
        error = win32_failure("OpenVirtualDisk", err);
        return false;
    }
    return true;
}

bool attach_handle(HANDLE handle, const bool writable, std::string& error) {
    ATTACH_VIRTUAL_DISK_PARAMETERS attach_params{};
    attach_params.Version = ATTACH_VIRTUAL_DISK_VERSION_1;

    const auto base_flags =
        writable ? ATTACH_VIRTUAL_DISK_FLAG_NONE : ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY;
    DWORD err = AttachVirtualDisk(
        handle, nullptr,
        static_cast<ATTACH_VIRTUAL_DISK_FLAG>(base_flags |
                                              ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER),
        0, &attach_params, nullptr);
    if (err != ERROR_SUCCESS) {
        err = AttachVirtualDisk(handle, nullptr, base_flags, 0, &attach_params, nullptr);
    }
    if (err != ERROR_SUCCESS) {
        error = win32_failure("AttachVirtualDisk", err);
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
        out.error = win32_failure("GetVirtualDiskPhysicalPath", err);
        return false;
    }
    out.windows_disk_number = parse_physical_drive_number(out.physical_path);
    if (out.windows_disk_number < 0) {
        out.error = "Could not parse physical drive number (physical_path=";
        out.error.append(path_diagnostic(out.physical_path));
        out.error.append(", path_chars=");
        out.error.append(std::to_string(out.physical_path.size()));
        out.error.push_back(')');
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

// One attach attempt: open/attach the VHDX with the requested media writability,
// resolve the surfaced physical disk, try to bring it online, and wait up to
// wait_rounds x 250 ms for its volumes (which may legitimately stay empty).
struct AttachAttempt {
    HANDLE handle{INVALID_HANDLE_VALUE};
    OnlineDiskOutcome online{};
    std::vector<VolumeInfo> volumes;
};

bool attach_and_collect(const std::wstring& vhdx_path, const bool writable,
                        const int wait_rounds, VhdAttachResult& out,
                        AttachAttempt& attempt, std::string& error) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (!open_vhdx(vhdx_path, writable, handle, error)) {
        return false;
    }
    if (!attach_handle(handle, writable, error)) {
        CloseHandle(handle);
        return false;
    }
    out.vhd_handle = handle;
    if (!resolve_physical_path(handle, out)) {
        error = out.error;
        (void)detach_vhd_handle(handle);
        out.vhd_handle = INVALID_HANDLE_VALUE;
        return false;
    }

    attempt.handle = handle;
    attempt.online = online_disk(out.windows_disk_number);
    for (int i = 0; i < wait_rounds; ++i) {
        attempt.volumes = collect_volumes_on_disk(out.windows_disk_number);
        if (!attempt.volumes.empty()) {
            break;
        }
        // Right after attach the disk can briefly refuse the online transition
        // (ERROR_WRITE_PROTECT); keep retrying until it takes or we time out.
        if (!attempt.online.set_attributes_ok) {
            attempt.online = online_disk(out.windows_disk_number);
        }
        Sleep(250);
    }
    return true;
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

    // Attempt 1 — read-only media (3 s): Windows 11 / Server 2022 online the disk
    // by themselves and mount its volumes read-only — nothing can ever write.
    // Attempt 2 — writable media (10 s): hosts that keep the disk OFFLINE (e.g.
    // Server 2019, SAN policy OfflineShared) reject the online transition for
    // read-only media (win32=19), so re-attach writable: the offline bit can be
    // cleared and the disk marked READ_ONLY *before* its volumes mount, so the
    // user still sees a read-only disk; stray writes land in the CoW overlay.
    AttachAttempt attempt;
    std::string ro_error;
    bool have_volumes = false;
    if (attach_and_collect(vhdx_path, /*writable=*/false, 12, out, attempt, ro_error)) {
        have_volumes = !attempt.volumes.empty();
        if (!have_volumes) {
            (void)detach_vhd_handle(attempt.handle);
            out.vhd_handle = INVALID_HANDLE_VALUE;
            attempt = AttachAttempt{};
            Sleep(250);
        }
    }
    bool used_writable_fallback = false;
    if (!have_volumes) {
        used_writable_fallback = true;
        if (!attach_and_collect(vhdx_path, /*writable=*/true, 40, out, attempt,
                                out.error)) {
            return false;
        }
    }

    HANDLE handle = attempt.handle;
    const auto online = attempt.online;
    std::vector<VolumeInfo>& volumes = attempt.volumes;

    auto data_vols = select_data_volumes(volumes, data_partitions);
    if (data_vols.empty()) {
        const auto state = query_disk_state(out.windows_disk_number);
        std::string detail = "No data volumes found after VHD attach (disk=";
        detail.append(std::to_string(out.windows_disk_number));
        detail.append(", attach=");
        detail.append(used_writable_fallback ? "rw_fallback" : "ro");
        detail.append(", volumes_on_disk=");
        detail.append(std::to_string(volumes.size()));
        detail.append(", online_disk=");
        if (!online.opened) {
            detail.append("open_failed:win32=");
            detail.append(std::to_string(online.open_error));
        } else if (!online.set_attributes_ok) {
            detail.append(online.writable ? "set_attrs_failed" : "read_only_handle");
            detail.append(":win32=");
            detail.append(std::to_string(online.set_attributes_error));
        } else {
            detail.append(online.writable ? "ok" : "ok_read_only_handle");
        }
        detail.append(", disk_state=");
        if (state.attributes_ok) {
            detail.append(state.offline ? "offline" : "online");
            if (state.disk_read_only) {
                detail.append("+read_only");
            }
        } else {
            detail.append("query_failed:win32=");
            detail.append(std::to_string(state.attributes_error));
        }
        detail.append(", partitions=");
        if (state.partition_count >= 0) {
            detail.append(std::to_string(state.partition_count));
        } else {
            detail.append("query_failed:win32=");
            detail.append(std::to_string(state.layout_error));
        }
        detail.push_back(')');
        if (state.attributes_ok && state.offline) {
            detail.append(
                "; disk stayed OFFLINE - likely disk signature/GPT GUID collision with an "
                "online disk (host restored from this image?)");
        }
        out.error = std::move(detail);
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
