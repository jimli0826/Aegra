#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::adapters::windows_disk {
namespace {

class UniqueVolumeFind final {
  public:
    explicit UniqueVolumeFind(const HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueVolumeFind() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            FindVolumeClose(handle_);
        }
    }

    UniqueVolumeFind(const UniqueVolumeFind&) = delete;
    UniqueVolumeFind& operator=(const UniqueVolumeFind&) = delete;
    UniqueVolumeFind(UniqueVolumeFind&&) = delete;
    UniqueVolumeFind& operator=(UniqueVolumeFind&&) = delete;

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

base::Result<std::string> to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return base::Result<std::string>::success({});
    }
    const auto input_size = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(),
                                              input_size, nullptr, 0, nullptr, nullptr);
    if (required == 0) {
        return base::Result<std::string>::failure(
            detail::win32_error(GetLastError(), "WideCharToMultiByte"));
    }

    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), input_size, output.data(),
                            required, nullptr, nullptr) == 0) {
        return base::Result<std::string>::failure(
            detail::win32_error(GetLastError(), "WideCharToMultiByte"));
    }
    return base::Result<std::string>::success(std::move(output));
}

std::vector<std::filesystem::path> mount_points(const wchar_t* volume_name) {
    DWORD required = 0;
    GetVolumePathNamesForVolumeNameW(volume_name, nullptr, 0, &required);
    if (required == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(required);
    if (!GetVolumePathNamesForVolumeNameW(volume_name, buffer.data(),
                                          static_cast<DWORD>(buffer.size()), &required)) {
        return {};
    }

    std::vector<std::filesystem::path> result;
    const auto names = std::span<const wchar_t>(buffer);
    std::size_t begin = 0;
    while (begin < names.size() && names[begin] != L'\0') {
        const auto remaining = names.subspan(begin);
        const auto terminator = std::ranges::find(remaining, L'\0');
        if (terminator == remaining.end()) {
            break;
        }
        result.emplace_back(std::wstring(remaining.begin(), terminator));
        begin += static_cast<std::size_t>(std::distance(remaining.begin(), terminator)) + 1U;
    }
    return result;
}

bool load_filesystem_metadata(const wchar_t* volume_name, WindowsVolumeInfo& info) {
    constexpr DWORD kTextBufferSize = 256;
    std::array<wchar_t, kTextBufferSize> label{};
    std::array<wchar_t, kTextBufferSize> filesystem{};
    DWORD serial_number = 0;
    DWORD maximum_component_length = 0;
    DWORD flags = 0;
    if (!GetVolumeInformationW(volume_name, label.data(), kTextBufferSize, &serial_number,
                               &maximum_component_length, &flags, filesystem.data(),
                               kTextBufferSize)) {
        return false;
    }

    DWORD sectors_per_cluster = 0;
    DWORD bytes_per_sector = 0;
    DWORD free_clusters = 0;
    DWORD total_clusters = 0;
    ULARGE_INTEGER free_bytes_available{};
    ULARGE_INTEGER total_bytes{};
    ULARGE_INTEGER total_free_bytes{};
    if (!GetDiskFreeSpaceW(volume_name, &sectors_per_cluster, &bytes_per_sector, &free_clusters,
                           &total_clusters) ||
        !GetDiskFreeSpaceExW(volume_name, &free_bytes_available, &total_bytes, &total_free_bytes)) {
        return false;
    }

    auto utf8_label = to_utf8(label.data());
    auto utf8_filesystem = to_utf8(filesystem.data());
    const auto cluster_size = static_cast<std::uint64_t>(sectors_per_cluster) * bytes_per_sector;
    if (!utf8_label || !utf8_filesystem ||
        cluster_size > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }

    info.label = std::move(utf8_label).value();
    info.filesystem = std::move(utf8_filesystem).value();
    // Free space is filesystem-scoped. Do not set total_size_bytes here: GetDiskFreeSpaceEx
    // capacity often differs from the raw partition length (EFI/FAT and some NTFS layouts).
    // Authoritative size comes from IOCTL_DISK_GET_LENGTH_INFO / extents / partition length.
    info.free_size_bytes = total_free_bytes.QuadPart;
    info.cluster_size_bytes = static_cast<std::uint32_t>(cluster_size);
    info.filesystem_metadata_available = true;
    info.is_read_only = (flags & FILE_READ_ONLY_VOLUME) != 0;
    // Stash filesystem capacity in total_size only as a last-resort seed; inspect_volume clears
    // volume_size_available until a raw size source confirms it (see apply_filesystem_capacity_fallback).
    if (total_bytes.QuadPart > 0) {
        info.total_size_bytes = total_bytes.QuadPart;
    }
    return true;
}

void apply_filesystem_capacity_fallback(WindowsVolumeInfo& info) {
    if (info.volume_size_available) {
        if (info.free_size_bytes > info.total_size_bytes) {
            info.free_size_bytes = info.total_size_bytes;
        }
        return;
    }
    if (info.total_size_bytes == 0) {
        return;
    }
    // Last resort for inventory display when raw length APIs are unavailable.
    info.volume_size_available = true;
    if (info.free_size_bytes > info.total_size_bytes) {
        info.free_size_bytes = info.total_size_bytes;
    }
}

struct PhysicalPartitionRange final {
    std::uint64_t offset_bytes{0};
    std::uint64_t size_bytes{0};
};

struct PhysicalDiskInfo final {
    std::uint32_t disk_number{0};
    std::uint64_t capacity_bytes{0};
    std::string partition_style{"Unknown"};
    std::string media_type{"Unknown"};
    /// Recognized partitions from DRIVE_LAYOUT (used to reject fake/cloud extents).
    std::vector<PhysicalPartitionRange> partitions;
};

// GPT Microsoft Reserved Partition type (E3C9E316-0B5C-4DB8-817D-F92DF00215AE).
inline constexpr GUID kMicrosoftReservedPartitionGuid = {
    0xE3C9E316, 0x0B5C, 0x4DB8, {0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE}};

[[nodiscard]] bool guid_equal(const GUID& left, const GUID& right) noexcept {
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

[[nodiscard]] detail::UniqueHandle open_physical_drive(const std::uint32_t disk_number) {
    const auto path = std::wstring(LR"(\\.\PhysicalDrive)") + std::to_wstring(disk_number);
    // Prefer query-only access; fall back to GENERIC_READ (old StorageManager path).
    detail::UniqueHandle handle(CreateFileW(path.c_str(), 0,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, 0, nullptr));
    if (handle.valid()) {
        return handle;
    }
    return detail::UniqueHandle(CreateFileW(path.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, 0, nullptr));
}

[[nodiscard]] std::string partition_style_name(const DWORD style) {
    switch (style) {
    case PARTITION_STYLE_MBR:
        return "MBR";
    case PARTITION_STYLE_GPT:
        return "GPT";
    case PARTITION_STYLE_RAW:
        return "RAW";
    default:
        return "Unknown";
    }
}

[[nodiscard]] std::optional<PhysicalDiskInfo> inspect_physical_disk(const std::uint32_t disk_number) {
    auto handle = open_physical_drive(disk_number);
    if (!handle.valid()) {
        return std::nullopt;
    }
    PhysicalDiskInfo info;
    info.disk_number = disk_number;

    GET_LENGTH_INFORMATION length{};
    DWORD bytes_returned = 0;
    if (DeviceIoControl(handle.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length,
                        sizeof(length), &bytes_returned, nullptr) &&
        bytes_returned >= sizeof(length) && length.Length.QuadPart >= 0) {
        info.capacity_bytes = static_cast<std::uint64_t>(length.Length.QuadPart);
    } else {
        DISK_GEOMETRY_EX geometry{};
        if (DeviceIoControl(handle.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry,
                            sizeof(geometry), &bytes_returned, nullptr) &&
            bytes_returned >= sizeof(geometry) && geometry.DiskSize.QuadPart >= 0) {
            info.capacity_bytes = static_cast<std::uint64_t>(geometry.DiskSize.QuadPart);
        }
    }

    // Drive layout needs room for many partitions (old StorageManager used 128).
    constexpr DWORD kMaxPartitions = 128;
    const auto layout_bytes =
        sizeof(DRIVE_LAYOUT_INFORMATION_EX) + sizeof(PARTITION_INFORMATION_EX) * kMaxPartitions;
    std::vector<std::byte> layout_buffer(layout_bytes);
    if (DeviceIoControl(handle.get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
                        layout_buffer.data(), static_cast<DWORD>(layout_buffer.size()),
                        &bytes_returned, nullptr) &&
        bytes_returned >= sizeof(DRIVE_LAYOUT_INFORMATION_EX)) {
        const auto* layout =
            reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(layout_buffer.data());
        info.partition_style = partition_style_name(layout->PartitionStyle);
        info.partitions.reserve(layout->PartitionCount);
        const auto max_parts = (std::min)(layout->PartitionCount, kMaxPartitions);
        for (DWORD index = 0; index < max_parts; ++index) {
            const auto& part = layout->PartitionEntry[index];
            if (part.PartitionLength.QuadPart <= 0 || part.StartingOffset.QuadPart < 0) {
                continue;
            }
            // Skip empty/container slots that layout APIs sometimes emit.
            if (part.PartitionStyle == PARTITION_STYLE_MBR && part.Mbr.PartitionType == 0) {
                continue;
            }
            info.partitions.push_back(PhysicalPartitionRange{
                static_cast<std::uint64_t>(part.StartingOffset.QuadPart),
                static_cast<std::uint64_t>(part.PartitionLength.QuadPart),
            });
        }
    }

    // Media type (old StorageManager GetDiskInfo): bus type + seek penalty.
    STORAGE_PROPERTY_QUERY query{};
    query.QueryType = PropertyStandardQuery;
    query.PropertyId = StorageDeviceProperty;
    std::array<std::byte, 1024> property_buffer{};
    if (DeviceIoControl(handle.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        property_buffer.data(), static_cast<DWORD>(property_buffer.size()),
                        &bytes_returned, nullptr) &&
        bytes_returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        STORAGE_DEVICE_DESCRIPTOR descriptor{};
        std::memcpy(&descriptor, property_buffer.data(), sizeof(descriptor));
        switch (descriptor.BusType) {
        case BusTypeNvme:
            info.media_type = "SSD";
            break;
        case BusTypeSata:
        case BusTypeAta:
            info.media_type = "HDD";
            break;
        case BusTypeUsb:
            info.media_type = "USB";
            break;
        case BusTypeVirtual:
        case BusTypeFileBackedVirtual:
            info.media_type = "Virtual";
            break;
        default:
            info.media_type = "Unknown";
            break;
        }
        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        DEVICE_SEEK_PENALTY_DESCRIPTOR seek_penalty{};
        if (DeviceIoControl(handle.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                            &seek_penalty, sizeof(seek_penalty), &bytes_returned, nullptr) &&
            bytes_returned >= sizeof(seek_penalty) && !seek_penalty.IncursSeekPenalty) {
            info.media_type = "SSD";
        }
    }
    return info;
}

// Enumerate physical disks first (old GetAllDisks): probe PhysicalDrive0..31.
[[nodiscard]] std::vector<PhysicalDiskInfo> enumerate_physical_disks() {
    constexpr std::uint32_t kMaxPhysicalDisks = 32;
    std::vector<PhysicalDiskInfo> disks;
    disks.reserve(8);
    for (std::uint32_t index = 0; index < kMaxPhysicalDisks; ++index) {
        if (auto disk = inspect_physical_disk(index)) {
            disks.push_back(std::move(*disk));
        }
    }
    return disks;
}

[[nodiscard]] std::optional<std::wstring> system_volume_root() {
    std::array<wchar_t, MAX_PATH + 1> windows_directory{};
    if (GetWindowsDirectoryW(windows_directory.data(),
                             static_cast<UINT>(windows_directory.size())) == 0) {
        return std::nullopt;
    }
    std::array<wchar_t, MAX_PATH + 1> root{};
    if (!GetVolumePathNameW(windows_directory.data(), root.data(),
                            static_cast<DWORD>(root.size()))) {
        return std::nullopt;
    }
    return std::wstring(root.data());
}

[[nodiscard]] bool equal_path(const std::filesystem::path& left, const std::wstring_view right) {
    return _wcsicmp(left.c_str(), std::wstring(right).c_str()) == 0;
}

[[nodiscard]] std::string source_id_for(const std::string_view stable_key) {
    const auto begin = stable_key.find('{');
    const auto end = stable_key.find('}', begin == std::string_view::npos ? 0 : begin + 1);
    if (begin == std::string_view::npos || end == std::string_view::npos || end <= begin + 1) {
        return {};
    }
    std::string id = "vol.";
    id.append(stable_key.substr(begin + 1, end - begin - 1));
    std::ranges::transform(id, id.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return id;
}

// Friendly volume titles aligned with old StorageManager::GetAllVolumes.
[[nodiscard]] std::string display_name_for(const WindowsVolumeInfo& volume, const bool is_system) {
    if (!volume.label.empty()) {
        return volume.label;
    }
    if (is_system) {
        return "System";
    }
    if (!volume.mount_points.empty()) {
        return "Local Disk";
    }
    return "Hidden Partition";
}

std::wstring volume_device_path(const std::wstring_view volume_name) {
    std::wstring path(volume_name);
    if (!path.empty() && path.back() == L'\\') {
        path.pop_back();
    }
    return path;
}

bool parse_extents(const std::vector<std::byte>& buffer, const DWORD bytes_returned,
                   WindowsVolumeInfo& info) {
    constexpr auto kHeaderSize = offsetof(VOLUME_DISK_EXTENTS, Extents);
    if (bytes_returned < kHeaderSize || bytes_returned > buffer.size()) {
        return false;
    }
    DWORD extent_count = 0;
    std::memcpy(&extent_count, buffer.data(), sizeof(extent_count));
    const auto extent_bytes = static_cast<std::uint64_t>(extent_count) * sizeof(DISK_EXTENT);
    if (extent_bytes > bytes_returned - kHeaderSize) {
        return false;
    }

    std::vector<WindowsVolumeExtent> extents;
    extents.reserve(extent_count);
    std::uint64_t extent_size_bytes = 0;
    const auto returned_data = std::span<const std::byte>(buffer).first(bytes_returned);
    for (DWORD index = 0; index < extent_count; ++index) {
        DISK_EXTENT extent{};
        const auto offset = kHeaderSize + static_cast<std::size_t>(index) * sizeof(extent);
        const auto encoded = returned_data.subspan(offset, sizeof(extent));
        std::memcpy(&extent, encoded.data(), sizeof(extent));
        if (extent.StartingOffset.QuadPart < 0 || extent.ExtentLength.QuadPart < 0) {
            return false;
        }
        const auto extent_length = static_cast<std::uint64_t>(extent.ExtentLength.QuadPart);
        if (extent_length > (std::numeric_limits<std::uint64_t>::max)() - extent_size_bytes) {
            return false;
        }
        extent_size_bytes += extent_length;
        extents.push_back(WindowsVolumeExtent{
            extent.DiskNumber,
            static_cast<std::uint64_t>(extent.StartingOffset.QuadPart),
            extent_length,
        });
    }
    info.extents = std::move(extents);
    info.disk_extents_available = true;
    if (!info.volume_size_available && extent_size_bytes > 0) {
        info.total_size_bytes = extent_size_bytes;
        info.volume_size_available = true;
    }
    return true;
}

void load_volume_size(const HANDLE handle, WindowsVolumeInfo& info) {
    GET_LENGTH_INFORMATION length{};
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length, sizeof(length),
                         &bytes_returned, nullptr) ||
        bytes_returned < sizeof(length) || length.Length.QuadPart <= 0) {
        return;
    }
    // Always prefer raw partition length over any earlier filesystem capacity estimate.
    info.total_size_bytes = static_cast<std::uint64_t>(length.Length.QuadPart);
    info.volume_size_available = true;
    if (info.free_size_bytes > info.total_size_bytes) {
        info.free_size_bytes = info.total_size_bytes;
    }
}

[[nodiscard]] detail::UniqueHandle open_volume_device(const std::wstring& device_path) {
    detail::UniqueHandle handle(CreateFileW(device_path.c_str(), 0,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, 0, nullptr));
    if (handle.valid()) {
        return handle;
    }
    // Match old StorageManager: GENERIC_READ when query-only open is denied.
    return detail::UniqueHandle(CreateFileW(device_path.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, 0, nullptr));
}

bool load_disk_extents(const wchar_t* volume_name, WindowsVolumeInfo& info) {
    const auto device_path = volume_device_path(volume_name);
    auto handle = open_volume_device(device_path);
    if (!handle.valid()) {
        return false;
    }
    load_volume_size(handle.get(), info);

    constexpr std::size_t kMaximumBufferSize = std::size_t{1024} * 1024U;
    std::vector<std::byte> buffer(sizeof(VOLUME_DISK_EXTENTS) + 8U * sizeof(DISK_EXTENT));
    while (buffer.size() <= kMaximumBufferSize) {
        DWORD bytes_returned = 0;
        if (DeviceIoControl(handle.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                            buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_returned,
                            nullptr)) {
            return parse_extents(buffer, bytes_returned, info);
        }
        if (GetLastError() != ERROR_MORE_DATA || buffer.size() == kMaximumBufferSize) {
            return false;
        }
        buffer.resize((std::min)(buffer.size() * 2U, kMaximumBufferSize));
    }
    return false;
}

// Hidden / unmounted partitions: name from GPT type (old EFI/MSR/Recovery mapping).
void load_partition_identity(const wchar_t* volume_name, WindowsVolumeInfo& info) {
    const auto device_path = volume_device_path(volume_name);
    auto handle = open_volume_device(device_path);
    if (!handle.valid()) {
        return;
    }
    PARTITION_INFORMATION_EX part{};
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(handle.get(), IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0, &part,
                         sizeof(part), &bytes_returned, nullptr) ||
        bytes_returned < sizeof(part) || part.PartitionLength.QuadPart < 0) {
        return;
    }
    if (!info.volume_size_available && part.PartitionLength.QuadPart > 0) {
        info.total_size_bytes = static_cast<std::uint64_t>(part.PartitionLength.QuadPart);
        info.volume_size_available = true;
    }
    // Only invent titles for volumes without a filesystem label / mount letter.
    if (!info.label.empty() || !info.mount_points.empty()) {
        return;
    }
    // GPT partition type GUIDs (same as old storage_manager.cpp).
    static constexpr GUID kEfiSystemPartition = {
        0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};
    static constexpr GUID kWindowsRecovery = {
        0xDE94BBA4, 0x06D1, 0x4D40, {0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC}};
    if (part.PartitionStyle == PARTITION_STYLE_GPT) {
        if (guid_equal(part.Gpt.PartitionType, kEfiSystemPartition)) {
            info.label = "EFI System Partition";
            if (info.filesystem.empty()) {
                info.filesystem = "FAT32";
            }
        } else if (guid_equal(part.Gpt.PartitionType, kMicrosoftReservedPartitionGuid)) {
            info.label = "Microsoft Reserved Partition";
            if (info.filesystem.empty()) {
                info.filesystem = "RAW";
            }
        } else if (guid_equal(part.Gpt.PartitionType, kWindowsRecovery)) {
            info.label = "Recovery Partition";
            if (info.filesystem.empty()) {
                info.filesystem = "NTFS";
            }
        } else {
            info.label = "Hidden Partition";
        }
    } else {
        info.label = "Hidden Partition";
    }
}

WindowsVolumeInfo inspect_volume(const wchar_t* volume_name) {
    WindowsVolumeInfo info;
    info.volume_guid_path = volume_name;
    info.mount_points = mount_points(volume_name);
    load_filesystem_metadata(volume_name, info);
    load_disk_extents(volume_name, info);
    load_partition_identity(volume_name, info);
    apply_filesystem_capacity_fallback(info);
    return info;
}

} // namespace

base::Result<std::vector<WindowsVolumeInfo>> WindowsVolumeEnumerator::enumerate() {
    constexpr DWORD kVolumeNameSize = MAX_PATH + 1;
    std::array<wchar_t, kVolumeNameSize> volume_name{};
    const auto find_handle = FindFirstVolumeW(volume_name.data(), kVolumeNameSize);
    if (find_handle == INVALID_HANDLE_VALUE) {
        return base::Result<std::vector<WindowsVolumeInfo>>::failure(
            detail::win32_error(GetLastError(), "FindFirstVolumeW"));
    }
    const UniqueVolumeFind find_guard(find_handle);

    std::vector<WindowsVolumeInfo> volumes;
    for (;;) {
        volumes.push_back(inspect_volume(volume_name.data()));
        if (FindNextVolumeW(find_handle, volume_name.data(), kVolumeNameSize)) {
            continue;
        }
        const auto error = GetLastError();
        if (error == ERROR_NO_MORE_FILES) {
            break;
        }
        return base::Result<std::vector<WindowsVolumeInfo>>::failure(
            detail::win32_error(error, "FindNextVolumeW"));
    }
    return base::Result<std::vector<WindowsVolumeInfo>>::success(std::move(volumes));
}

[[nodiscard]] std::string normalize_mount_letter(const WindowsVolumeInfo& volume) {
    if (volume.mount_points.empty()) {
        return {};
    }
    auto mount = to_utf8(volume.mount_points.front().wstring());
    if (!mount) {
        return {};
    }
    auto path = std::move(mount).value();
    while (!path.empty() && (path.back() == '\\' || path.back() == '/')) {
        path.pop_back();
    }
    return path;
}

bool supports_vss_snapshot(const WindowsVolumeInfo& volume) noexcept {
    std::string normalized(volume.filesystem);
    std::ranges::transform(normalized, normalized.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    // NTFS/ReFS are the primary VSS targets. FAT/FAT32/exFAT are included so whole-disk system
    // backups (EFI System Partition + OS volume) can share one snapshot set, matching the prior
    // product path that successfully shadow-copied both. RAW/unknown stay on the raw path.
    return normalized == "NTFS" || normalized == "REFS" || normalized == "FAT" ||
           normalized == "FAT32" || normalized == "EXFAT";
}

/// True when the volume's primary extent lands on a real partition of the physical disk
/// with a matching size. Cloud / filter volumes (e.g. Google Drive) often report a disk
/// number via IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS but do not correspond to any partition
/// in Disk Management — those must not appear under that Disk in the Backup source tree.
[[nodiscard]] bool volume_extent_matches_disk_partition(
    const WindowsVolumeInfo& volume, const PhysicalDiskInfo& disk) noexcept {
    if (volume.extents.empty() || disk.partitions.empty()) {
        return false;
    }
    const auto& extent = volume.extents.front();
    if (extent.disk_number != disk.disk_number || extent.length_bytes == 0) {
        return false;
    }
    // Prefer authoritative raw volume length when present; filesystem capacity can be smaller.
    const auto volume_size =
        volume.volume_size_available && volume.total_size_bytes > 0
            ? volume.total_size_bytes
            : extent.length_bytes;
    constexpr std::uint64_t kOffsetToleranceBytes = 2ULL * 1024ULL * 1024ULL; // 2 MiB
    for (const auto& part : disk.partitions) {
        if (part.size_bytes == 0) {
            continue;
        }
        const auto offset_delta = extent.disk_offset_bytes >= part.offset_bytes
                                      ? extent.disk_offset_bytes - part.offset_bytes
                                      : part.offset_bytes - extent.disk_offset_bytes;
        if (offset_delta > kOffsetToleranceBytes) {
            continue;
        }
        // Size must track the partition (not a 1 GiB cloud volume on a 900 GiB partition).
        const auto size_delta = volume_size >= part.size_bytes ? volume_size - part.size_bytes
                                                               : part.size_bytes - volume_size;
        const auto size_tolerance =
            (std::max)(16ULL * 1024ULL * 1024ULL, part.size_bytes / 100ULL); // 16 MiB or 1%
        if (size_delta <= size_tolerance) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_local_block_volume(const WindowsVolumeInfo& volume) noexcept {
    // Prefer a root path for GetDriveType; GUID path also works on modern Windows.
    std::wstring root;
    if (!volume.mount_points.empty()) {
        root = volume.mount_points.front().wstring();
    } else if (!volume.volume_guid_path.empty()) {
        root = volume.volume_guid_path.wstring();
    }
    if (root.empty()) {
        // Hidden partitions without a mount still participate in disk backup.
        return true;
    }
    if (root.back() != L'\\') {
        root.push_back(L'\\');
    }
    const auto drive_type = GetDriveTypeW(root.c_str());
    // Keep fixed + removable (USB data disks). Drop remote/CD/RAM/unknown cloud roots.
    return drive_type == DRIVE_FIXED || drive_type == DRIVE_REMOVABLE ||
           drive_type == DRIVE_NO_ROOT_DIR;
}

[[nodiscard]] ports::SourceInventoryRecord
make_disk_shell_record(const PhysicalDiskInfo& disk, const bool is_system_disk) {
    return ports::SourceInventoryRecord{
        .source_id = "disk." + std::to_string(disk.disk_number),
        .stable_key = std::string(R"(\\.\PhysicalDrive)") + std::to_string(disk.disk_number),
        .display_name = "Disk " + std::to_string(disk.disk_number),
        .kind = contracts::SourceKind::kVolume,
        .availability = contracts::SourceAvailability::kAvailable,
        .capacity_bytes = 0,
        .free_bytes = 0,
        .disk_capacity_bytes = disk.capacity_bytes,
        .is_system = is_system_disk,
        .is_read_only = true,
        .disk_number = disk.disk_number,
        .offset_bytes = 0,
        .mount_letter = {},
        .volume_label = {},
        .health_status = "Unallocated",
        .partition_style = disk.partition_style,
        .media_type = disk.media_type,
    };
}

[[nodiscard]] base::Result<ports::SourceInventoryRecord>
make_volume_record(const WindowsVolumeInfo& volume, const PhysicalDiskInfo& disk,
                   const std::optional<std::wstring>& system_root) {
    auto stable_key = to_utf8(volume.volume_guid_path.wstring());
    if (!stable_key) {
        return base::Result<ports::SourceInventoryRecord>::failure(stable_key.error());
    }
    auto source_id = source_id_for(stable_key.value());
    if (source_id.empty()) {
        return base::Result<ports::SourceInventoryRecord>::failure(
            {base::ErrorCode::kInvalidArgument, "volume stable key is not a volume GUID path"});
    }
    const bool is_system =
        system_root && std::ranges::any_of(volume.mount_points, [&](const auto& mount) {
            return equal_path(mount, *system_root);
        });
    auto mount_letter = normalize_mount_letter(volume);
    std::string health = "Healthy";
    if (is_system) {
        health = "Healthy (Boot, System)";
    } else if (mount_letter.empty()) {
        health = "Healthy (Hidden)";
    }
    if (!volume.volume_size_available) {
        health = "Unavailable";
    } else if (!supports_vss_snapshot(volume)) {
        health += " - Raw backup";
    }
    auto display_name = display_name_for(volume, is_system);
    // Prefer the friendly title for both fields so Desktop name binding matches old UI.
    const auto volume_label = display_name;
    auto free_bytes = volume.free_size_bytes;
    if (free_bytes > volume.total_size_bytes) {
        free_bytes = volume.total_size_bytes;
    }
    // Prefer the matched partition start for layout bars; fall back to the primary extent.
    std::uint64_t offset_bytes = 0;
    if (!volume.extents.empty()) {
        offset_bytes = volume.extents.front().disk_offset_bytes;
        constexpr std::uint64_t kOffsetToleranceBytes = 1024ULL * 1024ULL;
        for (const auto& part : disk.partitions) {
            const auto delta = offset_bytes >= part.offset_bytes
                                   ? offset_bytes - part.offset_bytes
                                   : part.offset_bytes - offset_bytes;
            if (delta <= kOffsetToleranceBytes) {
                offset_bytes = part.offset_bytes;
                break;
            }
        }
    }
    return base::Result<ports::SourceInventoryRecord>::success(ports::SourceInventoryRecord{
        .source_id = std::move(source_id),
        .stable_key = std::move(stable_key).value(),
        .display_name = std::move(display_name),
        .kind = contracts::SourceKind::kVolume,
        .availability = volume.volume_size_available
                            ? contracts::SourceAvailability::kAvailable
                            : contracts::SourceAvailability::kUnavailable,
        .capacity_bytes = volume.total_size_bytes,
        .free_bytes = free_bytes,
        .disk_capacity_bytes = disk.capacity_bytes,
        .is_system = is_system,
        .is_read_only = volume.is_read_only,
        .disk_number = disk.disk_number,
        .offset_bytes = offset_bytes,
        .mount_letter = std::move(mount_letter),
        .volume_label = volume_label,
        .health_status = std::move(health),
        .partition_style = disk.partition_style,
        .media_type = disk.media_type.empty() ? "Unknown" : disk.media_type,
    });
}

base::Result<std::vector<ports::SourceInventoryRecord>>
WindowsSourceInventory::list_sources(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::vector<ports::SourceInventoryRecord>>::failure(
            {base::ErrorCode::kCancelled, "volume inventory cancelled"});
    }
    // Disk-first inventory (old GetDisksWithVolumes): physical drives are the tree roots.
    const auto disks = enumerate_physical_disks();
    std::map<std::uint32_t, PhysicalDiskInfo> disk_by_number;
    for (const auto& disk : disks) {
        disk_by_number.emplace(disk.disk_number, disk);
    }

    auto volumes = WindowsVolumeEnumerator::enumerate();
    if (!volumes) {
        return base::Result<std::vector<ports::SourceInventoryRecord>>::failure(volumes.error());
    }
    const auto system_root = system_volume_root();
    std::vector<ports::SourceInventoryRecord> records;
    records.reserve(volumes.value().size() + disks.size());
    std::map<std::uint32_t, bool> system_by_disk;

    for (const auto& volume : volumes.value()) {
        if (cancellation.stop_requested()) {
            return base::Result<std::vector<ports::SourceInventoryRecord>>::failure(
                {base::ErrorCode::kCancelled, "volume inventory cancelled"});
        }
        // Old GetAllVolumes: skip volumes without a resolvable disk number (never default to 0).
        if (!volume.disk_extents_available || volume.extents.empty()) {
            continue;
        }
        if (!is_local_block_volume(volume)) {
            continue;
        }
        const auto disk_number = volume.extents.front().disk_number;
        auto disk_it = disk_by_number.find(disk_number);
        // Require an openable PhysicalDrive whose partition table contains this extent.
        // Prevents cloud/filter volumes (e.g. Google Drive) from nesting under a real Disk.
        if (disk_it == disk_by_number.end()) {
            continue;
        }
        const auto& disk_info = disk_it->second;
        if (!volume_extent_matches_disk_partition(volume, disk_info)) {
            continue;
        }
        auto record = make_volume_record(volume, disk_info, system_root);
        if (!record) {
            // Skip unusable GUID identities; do not fail the whole inventory.
            if (record.error().code == base::ErrorCode::kInvalidArgument) {
                continue;
            }
            return base::Result<std::vector<ports::SourceInventoryRecord>>::failure(record.error());
        }
        if (record.value().is_system) {
            system_by_disk[disk_number] = true;
        }
        records.push_back(std::move(record).value());
    }

    // Always publish disk.N shells for every PhysicalDrive:
    // - empty disks: Desktop disksTree Unallocated rows (old GetDisksWithVolumes);
    // - disks with volumes: restore target identity (PrepareRestore target_source_id = disk.N).
    // capacity_bytes stays 0 so volume backup selectability ignores shells.
    for (const auto& disk : disks) {
        const bool is_system = system_by_disk.contains(disk.disk_number);
        records.push_back(make_disk_shell_record(disk, is_system));
    }

    std::ranges::sort(records, {}, &ports::SourceInventoryRecord::source_id);
    return base::Result<std::vector<ports::SourceInventoryRecord>>::success(std::move(records));
}

} // namespace aegra::adapters::windows_disk
