#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_disk {
namespace {

// Match old BackupEngine: never treat the first 64 MiB of NTFS/ReFS as free.
constexpr std::uint64_t kNtfsSystemAreaBytes = 64ULL * 1024ULL * 1024ULL;

enum class FreeSkipKind {
    kNone,
    kNtfsLinear,
    kFatRemapped,
};

[[nodiscard]] std::string normalize_filesystem(const std::string_view filesystem) {
    std::string value(filesystem);
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

[[nodiscard]] FreeSkipKind free_skip_kind(const std::string_view filesystem) {
    const auto name = normalize_filesystem(filesystem);
    if (name == "NTFS" || name == "REFS") {
        return FreeSkipKind::kNtfsLinear;
    }
    if (name == "FAT" || name == "FAT32") {
        return FreeSkipKind::kFatRemapped;
    }
    return FreeSkipKind::kNone;
}

[[nodiscard]] std::wstring device_open_path(const std::filesystem::path& path) {
    auto value = path.native();
    if (!value.empty() && value.back() == L'\\') {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] bool is_cluster_allocated(const std::vector<std::uint8_t>& bits,
                                        const std::uint64_t cluster_index) noexcept {
    const auto byte_index = cluster_index / 8U;
    if (byte_index >= bits.size()) {
        // Missing bits: treat as used (safe — may enlarge backup).
        return true;
    }
    return (bits[static_cast<std::size_t>(byte_index)] &
            static_cast<std::uint8_t>(1U << (cluster_index % 8U))) != 0;
}

void append_free_range(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges,
                       const std::uint64_t start, const std::uint64_t end) {
    if (end <= start) {
        return;
    }
    if (!ranges.empty() && ranges.back().second == start) {
        ranges.back().second = end;
        return;
    }
    ranges.emplace_back(start, end);
}

#pragma pack(push, 1)
struct FatBootSectorLite final {
    std::uint8_t jump[3];
    char oem[8];
    std::uint16_t bytes_per_sec;
    std::uint8_t sec_per_clus;
    std::uint16_t rsvd_sec_cnt;
    std::uint8_t num_fats;
    std::uint16_t root_ent_cnt;
    std::uint16_t tot_sec16;
    std::uint8_t media;
    std::uint16_t fat_sz16;
    std::uint16_t sec_per_trk;
    std::uint16_t num_heads;
    std::uint32_t hidd_sec;
    std::uint32_t tot_sec32;
    std::uint32_t fat_sz32;
};
#pragma pack(pop)

struct FatGeometry final {
    std::uint64_t data_area_offset_bytes{0};
    std::uint64_t cluster_size_bytes{0};
    std::uint64_t hint_total_clusters{0};
};

[[nodiscard]] std::optional<FatGeometry> read_fat_geometry(const std::wstring& open_path,
                                                           const std::uint64_t volume_size) {
    detail::UniqueHandle handle(CreateFileW(open_path.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, 0, nullptr));
    if (!handle.valid()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> boot(4096, 0);
    DWORD bytes_read = 0;
    if (!ReadFile(handle.get(), boot.data(), static_cast<DWORD>(boot.size()), &bytes_read,
                  nullptr) ||
        bytes_read < 512) {
        return std::nullopt;
    }
    if (boot[510] != 0x55 || boot[511] != 0xAA) {
        return std::nullopt;
    }
    FatBootSectorLite bpb{};
    std::memcpy(&bpb, boot.data(), sizeof(bpb));
    if (bpb.bytes_per_sec == 0 || (bpb.bytes_per_sec & (bpb.bytes_per_sec - 1U)) != 0 ||
        bpb.sec_per_clus == 0 || bpb.num_fats == 0) {
        return std::nullopt;
    }
    const auto fat_sz = bpb.fat_sz16 != 0 ? static_cast<std::uint32_t>(bpb.fat_sz16) : bpb.fat_sz32;
    if (fat_sz == 0) {
        return std::nullopt;
    }
    const auto root_dir_sectors =
        (static_cast<std::uint32_t>(bpb.root_ent_cnt) * 32U + bpb.bytes_per_sec - 1U) /
        bpb.bytes_per_sec;
    const auto first_data_sector =
        bpb.fat_sz16 != 0
            ? static_cast<std::uint32_t>(bpb.rsvd_sec_cnt) +
                  static_cast<std::uint32_t>(bpb.num_fats) * fat_sz + root_dir_sectors
            : static_cast<std::uint32_t>(bpb.rsvd_sec_cnt) +
                  static_cast<std::uint32_t>(bpb.num_fats) * fat_sz;
    if (first_data_sector == 0) {
        return std::nullopt;
    }
    FatGeometry geometry;
    geometry.cluster_size_bytes =
        static_cast<std::uint64_t>(bpb.sec_per_clus) * bpb.bytes_per_sec;
    geometry.data_area_offset_bytes =
        static_cast<std::uint64_t>(first_data_sector) * bpb.bytes_per_sec;
    if (geometry.cluster_size_bytes == 0 || geometry.data_area_offset_bytes >= volume_size) {
        return std::nullopt;
    }
    geometry.hint_total_clusters =
        (volume_size - geometry.data_area_offset_bytes) / geometry.cluster_size_bytes;
    return geometry;
}

struct VolumeBitmap final {
    std::vector<std::uint8_t> bits;
    std::uint64_t cluster_size_bytes{0};
    std::uint64_t total_clusters{0};
    std::uint64_t starting_lcn{0};
};

[[nodiscard]] std::optional<VolumeBitmap>
query_volume_bitmap(const std::wstring& open_path, const std::uint64_t hint_cluster_size,
                    const std::uint64_t hint_total_clusters) {
    detail::UniqueHandle handle(CreateFileW(open_path.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, 0, nullptr));
    if (!handle.valid()) {
        return std::nullopt;
    }

    VolumeBitmap result;
    std::wstring path_for_size = open_path;
    if (path_for_size.empty() || path_for_size.back() != L'\\') {
        path_for_size.push_back(L'\\');
    }
    DWORD sectors_per_cluster = 0;
    DWORD bytes_per_sector = 0;
    DWORD free_clusters = 0;
    DWORD total_clusters32 = 0;
    if (GetDiskFreeSpaceW(path_for_size.c_str(), &sectors_per_cluster, &bytes_per_sector,
                          &free_clusters, &total_clusters32) &&
        sectors_per_cluster > 0 && bytes_per_sector > 0 && total_clusters32 > 0) {
        result.cluster_size_bytes =
            static_cast<std::uint64_t>(sectors_per_cluster) * bytes_per_sector;
        result.total_clusters = total_clusters32;
    } else if (hint_cluster_size > 0 && hint_total_clusters > 0) {
        result.cluster_size_bytes = hint_cluster_size;
        result.total_clusters = hint_total_clusters;
    } else {
        return std::nullopt;
    }

    constexpr std::size_t kHeaderBytes = 16;
    constexpr std::uint64_t kChunkClusters = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t next_lcn = 0;
    std::uint64_t clusters_collected = 0;
    bool have_starting = false;

    while (clusters_collected < result.total_clusters) {
        STARTING_LCN_INPUT_BUFFER input{};
        input.StartingLcn.QuadPart = static_cast<LONGLONG>(next_lcn);
        const auto remaining = result.total_clusters - clusters_collected;
        const auto request_clusters = (std::min)(remaining, kChunkClusters);
        const auto request_bytes = (request_clusters + 7U) / 8U;
        std::vector<std::uint8_t> temp(kHeaderBytes + static_cast<std::size_t>(request_bytes) + 64);
        DWORD bytes_returned = 0;
        const auto ok =
            DeviceIoControl(handle.get(), FSCTL_GET_VOLUME_BITMAP, &input, sizeof(input),
                            temp.data(), static_cast<DWORD>(temp.size()), &bytes_returned, nullptr);
        const auto error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error != ERROR_MORE_DATA) {
            return std::nullopt;
        }
        if (bytes_returned <= kHeaderBytes) {
            return std::nullopt;
        }
        LARGE_INTEGER returned_start{};
        LARGE_INTEGER returned_size{};
        std::memcpy(&returned_start, temp.data(), sizeof(returned_start));
        std::memcpy(&returned_size, temp.data() + 8, sizeof(returned_size));
        if (!have_starting) {
            if (returned_start.QuadPart < 0) {
                return std::nullopt;
            }
            result.starting_lcn = static_cast<std::uint64_t>(returned_start.QuadPart);
            have_starting = true;
        }
        if (returned_size.QuadPart <= 0) {
            return std::nullopt;
        }
        const auto bit_count = static_cast<std::uint64_t>(returned_size.QuadPart);
        const auto data_bytes = static_cast<std::size_t>((bit_count + 7U) / 8U);
        const auto available = static_cast<std::size_t>(bytes_returned - kHeaderBytes);
        if (data_bytes > available) {
            return std::nullopt;
        }
        result.bits.insert(result.bits.end(), temp.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes),
                           temp.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + data_bytes));
        clusters_collected += bit_count;
        next_lcn = static_cast<std::uint64_t>(returned_start.QuadPart) + bit_count;
        if (bit_count < request_clusters || error != ERROR_MORE_DATA) {
            break;
        }
    }
    if (clusters_collected == 0) {
        return std::nullopt;
    }
    result.total_clusters = clusters_collected;
    return result;
}

void add_cluster_free_range(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges,
                            const std::uint64_t volume_size, const std::uint64_t protected_prefix,
                            const std::uint64_t start_byte, const std::uint64_t end_byte) {
    auto start = start_byte;
    auto end = end_byte;
    if (start < protected_prefix) {
        start = protected_prefix;
    }
    if (end > volume_size) {
        end = volume_size;
    }
    append_free_range(ranges, start, end);
}

[[nodiscard]] FreeSkipPlan
build_ntfs_plan(const VolumeBitmap& bitmap, const std::uint64_t volume_size) {
    FreeSkipPlan plan;
    plan.total_bytes = volume_size;
    plan.protected_prefix_bytes = (std::min)(kNtfsSystemAreaBytes, volume_size);
    plan.filesystem = "NTFS";
    for (std::uint64_t index = 0; index < bitmap.total_clusters; ++index) {
        if (is_cluster_allocated(bitmap.bits, index)) {
            continue;
        }
        const auto lcn = bitmap.starting_lcn + index;
        if (lcn > (std::numeric_limits<std::uint64_t>::max)() / bitmap.cluster_size_bytes) {
            continue;
        }
        const auto start = lcn * bitmap.cluster_size_bytes;
        const auto end = start + bitmap.cluster_size_bytes;
        add_cluster_free_range(plan.free_ranges, volume_size, plan.protected_prefix_bytes, start,
                               end);
    }
    for (const auto& range : plan.free_ranges) {
        plan.free_bytes += range.second - range.first;
    }
    plan.applied = !plan.free_ranges.empty();
    return plan;
}

[[nodiscard]] FreeSkipPlan build_fat_plan(const VolumeBitmap& bitmap, const FatGeometry& geometry,
                                          const std::uint64_t volume_size) {
    FreeSkipPlan plan;
    plan.total_bytes = volume_size;
    plan.protected_prefix_bytes = geometry.data_area_offset_bytes;
    plan.filesystem = "FAT";
    const auto cluster_size = bitmap.cluster_size_bytes != 0 ? bitmap.cluster_size_bytes
                                                             : geometry.cluster_size_bytes;
    if (cluster_size == 0) {
        return plan;
    }
    for (std::uint64_t index = 0; index < bitmap.total_clusters; ++index) {
        if (is_cluster_allocated(bitmap.bits, index)) {
            continue;
        }
        const auto data_cluster = bitmap.starting_lcn + index;
        if (data_cluster > (std::numeric_limits<std::uint64_t>::max)() / cluster_size) {
            continue;
        }
        const auto start = geometry.data_area_offset_bytes + data_cluster * cluster_size;
        const auto end = start + cluster_size;
        // Reserved + FAT stay used (start always >= data_area_offset).
        add_cluster_free_range(plan.free_ranges, volume_size, geometry.data_area_offset_bytes,
                               start, end);
    }
    for (const auto& range : plan.free_ranges) {
        plan.free_bytes += range.second - range.first;
    }
    plan.applied = !plan.free_ranges.empty();
    return plan;
}

// First index with range.first > offset (or size if none).
[[nodiscard]] std::size_t first_range_after(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges,
    const std::uint64_t offset) noexcept {
    std::size_t low = 0;
    std::size_t high = ranges.size();
    while (low < high) {
        const auto mid = low + (high - low) / 2U;
        if (ranges[mid].first > offset) {
            high = mid;
        } else {
            low = mid + 1U;
        }
    }
    return low;
}

[[nodiscard]] std::uint64_t free_length_at(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, const std::uint64_t offset,
    const std::uint64_t limit) noexcept {
    if (limit == 0 || ranges.empty()) {
        return 0;
    }
    const auto after = first_range_after(ranges, offset);
    if (after == 0) {
        return 0;
    }
    const auto& range = ranges[after - 1U];
    if (offset < range.first || offset >= range.second) {
        return 0;
    }
    return (std::min)(range.second - offset, limit);
}

[[nodiscard]] std::uint64_t used_length_at(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, const std::uint64_t offset,
    const std::uint64_t limit) noexcept {
    if (limit == 0) {
        return 0;
    }
    if (ranges.empty()) {
        return limit;
    }
    const auto after = first_range_after(ranges, offset);
    if (after > 0) {
        const auto& previous = ranges[after - 1U];
        if (offset >= previous.first && offset < previous.second) {
            return 0;
        }
    }
    if (after < ranges.size() && ranges[after].first > offset) {
        return (std::min)(ranges[after].first - offset, limit);
    }
    return limit;
}

base::Result<std::size_t> read_used_range(ports::IBlockSource& inner, const std::uint64_t offset,
                                          const std::span<std::byte> destination,
                                          const base::CancellationToken& cancellation) {
    std::size_t filled = 0;
    while (filled < destination.size()) {
        auto result =
            inner.read(offset + filled, destination.subspan(filled), cancellation);
        if (!result) {
            return result;
        }
        if (result.value() == 0) {
            return base::Result<std::size_t>::failure(base::Error{
                base::ErrorCode::kIoFailure,
                "underlying block source returned zero bytes before end of used range",
            });
        }
        filled += result.value();
    }
    return base::Result<std::size_t>::success(filled);
}

} // namespace

[[nodiscard]] std::wstring trailing_slash_path(const std::filesystem::path& path) {
    auto value = path.native();
    if (value.empty()) {
        return value;
    }
    if (value.back() != L'\\' && value.back() != L'/') {
        value.push_back(L'\\');
    }
    return value;
}

[[nodiscard]] std::wstring strip_trailing_separators(std::wstring value) {
    while (!value.empty() && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    return value;
}

// Normalize VSS device object forms so file opens use a Win32-visible root.
// Accepts:
//   \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN
//   \Device\HarddiskVolumeShadowCopyN
//   \\.\HarddiskVolumeShadowCopyN  (rare)
[[nodiscard]] std::wstring normalize_volume_root_for_files(const std::filesystem::path& device_path) {
    auto value = strip_trailing_separators(device_path.native());
    if (value.empty()) {
        return value;
    }
    constexpr std::wstring_view kGlobalRoot = L"\\\\?\\GLOBALROOT";
    constexpr std::wstring_view kDevice = L"\\Device\\";
    if (value.size() >= kDevice.size() &&
        _wcsnicmp(value.c_str(), kDevice.data(), kDevice.size()) == 0) {
        value = std::wstring(kGlobalRoot) + value;
    }
    return value;
}

// Target for DefineDosDevice(DDD_RAW_TARGET_PATH): "\Device\HarddiskVolumeShadowCopyN"
[[nodiscard]] std::optional<std::wstring>
nt_device_path_for_dos_map(const std::wstring& normalized_root) {
    constexpr std::wstring_view kGlobalRootDevice = L"\\\\?\\GLOBALROOT\\Device\\";
    if (normalized_root.size() > kGlobalRootDevice.size() &&
        _wcsnicmp(normalized_root.c_str(), kGlobalRootDevice.data(),
                  kGlobalRootDevice.size()) == 0) {
        return std::wstring(L"\\Device\\") + normalized_root.substr(kGlobalRootDevice.size());
    }
    constexpr std::wstring_view kDevice = L"\\Device\\";
    if (normalized_root.size() > kDevice.size() &&
        _wcsnicmp(normalized_root.c_str(), kDevice.data(), kDevice.size()) == 0) {
        return normalized_root;
    }
    return std::nullopt;
}

// Temporary DOS device (AipCopy MapDriveLetter / numeric drive style) so Win32 can open
// files under a VSS shadow copy when the GLOBALROOT path form fails.
class TemporaryDosDeviceMap final {
  public:
    TemporaryDosDeviceMap() = default;
    ~TemporaryDosDeviceMap() { release(); }

    TemporaryDosDeviceMap(const TemporaryDosDeviceMap&) = delete;
    TemporaryDosDeviceMap& operator=(const TemporaryDosDeviceMap&) = delete;
    TemporaryDosDeviceMap(TemporaryDosDeviceMap&& other) noexcept
        : dos_name_(std::move(other.dos_name_)), target_(std::move(other.target_)),
          active_(other.active_) {
        other.active_ = false;
    }
    TemporaryDosDeviceMap& operator=(TemporaryDosDeviceMap&& other) noexcept {
        if (this != &other) {
            release();
            dos_name_ = std::move(other.dos_name_);
            target_ = std::move(other.target_);
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }

    [[nodiscard]] static TemporaryDosDeviceMap try_map(const std::wstring& nt_device_path) {
        TemporaryDosDeviceMap map;
        if (nt_device_path.empty()) {
            return map;
        }
        // Prefer free A–Z: names that do not collide with assigned drive letters.
        for (wchar_t letter = L'Z'; letter >= L'A'; --letter) {
            const std::wstring drive_root = std::wstring(1, letter) + L":\\";
            if (GetDriveTypeW(drive_root.c_str()) != DRIVE_NO_ROOT_DIR) {
                continue;
            }
            const std::wstring dos_name = std::wstring(1, letter) + L":";
            if (!DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM, dos_name.c_str(),
                                  nt_device_path.c_str())) {
                continue;
            }
            map.dos_name_ = dos_name;
            map.target_ = nt_device_path;
            map.active_ = true;
            return map;
        }
        return map;
    }

    [[nodiscard]] bool active() const noexcept { return active_; }

    // "\\.\X:" volume device (no trailing slash)
    [[nodiscard]] std::wstring win32_device_path() const {
        if (!active_) {
            return {};
        }
        return L"\\\\.\\" + dos_name_;
    }

    // "\\.\X:\" root for concatenating file names
    [[nodiscard]] std::wstring win32_root_with_slash() const {
        auto device = win32_device_path();
        if (!device.empty()) {
            device.push_back(L'\\');
        }
        return device;
    }

    void release() noexcept {
        if (!active_) {
            return;
        }
        DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE |
                             DDD_NO_BROADCAST_SYSTEM,
                         dos_name_.c_str(), target_.c_str());
        active_ = false;
        dos_name_.clear();
        target_.clear();
    }

  private:
    std::wstring dos_name_;
    std::wstring target_;
    bool active_{false};
};

struct FileExtent final {
    std::uint64_t lcn{0};
    std::uint64_t length_clusters{0};
};

[[nodiscard]] detail::UniqueHandle open_file_for_extents(const std::wstring& file_path) {
    // pagefile.sys is often share-locked on live volumes; try attribute/query opens first
    // (old DiskDevice path), then backup-semantics opens (service/SYSTEM / snapshot).
    // Snapshot-homologous opens (AipCopy CreateFile access=0 + BACKUP_SEMANTICS) are first.
    constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD access_modes[] = {static_cast<DWORD>(0), FILE_READ_ATTRIBUTES, GENERIC_READ};
    const DWORD flag_modes[] = {FILE_FLAG_BACKUP_SEMANTICS, 0, FILE_FLAG_BACKUP_SEMANTICS};
    for (const auto access : access_modes) {
        for (const auto flags : flag_modes) {
            detail::UniqueHandle handle(CreateFileW(file_path.c_str(), access, kShare, nullptr,
                                                    OPEN_EXISTING, flags, nullptr));
            if (handle.valid()) {
                return handle;
            }
        }
    }
    return {};
}

[[nodiscard]] bool query_file_extents_via_handle(const HANDLE handle,
                                                 std::vector<FileExtent>& extents) {
    STARTING_VCN_INPUT_BUFFER input{};
    input.StartingVcn.QuadPart = 0;
    std::vector<std::uint8_t> buffer(16U * 1024U);
    for (;;) {
        DWORD bytes_returned = 0;
        const auto ok = DeviceIoControl(handle, FSCTL_GET_RETRIEVAL_POINTERS, &input, sizeof(input),
                                        buffer.data(), static_cast<DWORD>(buffer.size()),
                                        &bytes_returned, nullptr);
        const auto error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error != ERROR_MORE_DATA) {
            return error == ERROR_HANDLE_EOF;
        }
        if (bytes_returned < sizeof(DWORD) + sizeof(LARGE_INTEGER)) {
            break;
        }
        const auto* output = reinterpret_cast<const RETRIEVAL_POINTERS_BUFFER*>(buffer.data());
        if (output->ExtentCount == 0) {
            break;
        }
        auto current_vcn = static_cast<std::uint64_t>(output->StartingVcn.QuadPart);
        for (DWORD index = 0; index < output->ExtentCount; ++index) {
            const auto next_vcn =
                static_cast<std::uint64_t>(output->Extents[index].NextVcn.QuadPart);
            const auto lcn = static_cast<std::uint64_t>(output->Extents[index].Lcn.QuadPart);
            const auto length = next_vcn - current_vcn;
            if (lcn != static_cast<std::uint64_t>(-1) && length > 0) {
                extents.push_back(FileExtent{lcn, length});
            }
            current_vcn = next_vcn;
        }
        if (ok) {
            break;
        }
        input.StartingVcn.QuadPart = static_cast<LONGLONG>(current_vcn);
    }
    return true;
}

// Volume open path for FSCTL_GET_NTFS_FILE_RECORD from a file path under that volume root.
// Supports Volume GUID, VSS GLOBALROOT, and temporary DOS-mapped \\.\X:\file forms.
[[nodiscard]] std::wstring volume_device_from_file_path(const std::wstring& file_path) {
    const auto last_slash = file_path.find_last_of(L"\\/");
    if (last_slash == std::wstring::npos || last_slash == 0) {
        return {};
    }
    auto root = file_path.substr(0, last_slash);
    // Volume GUID: \\?\Volume{guid}\name → device \\?\Volume{guid}
    if (root.size() >= 11 &&
        (_wcsnicmp(root.c_str(), L"\\\\?\\Volume{", 11) == 0 ||
         _wcsnicmp(root.c_str(), L"\\\\.\\Volume{", 11) == 0)) {
        const auto volume_end = root.find(L'}', 11);
        if (volume_end == std::wstring::npos) {
            return {};
        }
        return root.substr(0, volume_end + 1);
    }
    // DOS-mapped or device root ends with "X:" → keep as volume open path.
    return strip_trailing_separators(std::move(root));
}

// Old DiskDevice::GetFileExtentsViaMFT — used when pagefile.sys is share-locked.
[[nodiscard]] bool query_file_extents_via_mft(const std::wstring& file_path,
                                              std::vector<FileExtent>& extents) {
    extents.clear();
    const auto last_slash = file_path.find_last_of(L"\\/");
    if (last_slash == std::wstring::npos || last_slash + 1 >= file_path.size()) {
        return false;
    }
    const auto directory = file_path.substr(0, last_slash + 1);
    const auto file_name = file_path.substr(last_slash + 1);
    const auto volume_device = volume_device_from_file_path(file_path);
    if (volume_device.empty()) {
        return false;
    }

    detail::UniqueHandle dir(CreateFileW(directory.c_str(), FILE_LIST_DIRECTORY,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                         nullptr));
    if (!dir.valid()) {
        return false;
    }
    LARGE_INTEGER file_id{};
    bool found = false;
    std::vector<std::uint8_t> info_buffer(32U * 1024U);
    for (;;) {
        if (!GetFileInformationByHandleEx(dir.get(), FileIdBothDirectoryInfo, info_buffer.data(),
                                          static_cast<DWORD>(info_buffer.size()))) {
            if (GetLastError() == ERROR_NO_MORE_FILES) {
                break;
            }
            return false;
        }
        auto* info = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(info_buffer.data());
        for (;;) {
            std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            if (_wcsicmp(name.c_str(), file_name.c_str()) == 0) {
                file_id = info->FileId;
                found = true;
                break;
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            info = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<std::uint8_t*>(info) +
                                                            info->NextEntryOffset);
        }
        if (found) {
            break;
        }
    }
    if (!found) {
        return false;
    }

    detail::UniqueHandle volume(CreateFileW(volume_device.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, 0, nullptr));
    if (!volume.valid()) {
        return false;
    }
    NTFS_FILE_RECORD_INPUT_BUFFER input{};
    input.FileReferenceNumber = file_id;
    std::vector<std::uint8_t> record_buffer(4096);
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(volume.get(), FSCTL_GET_NTFS_FILE_RECORD, &input, sizeof(input),
                         record_buffer.data(), static_cast<DWORD>(record_buffer.size()),
                         &bytes_returned, nullptr)) {
        return false;
    }
    auto* output = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(record_buffer.data());
    auto* record = output->FileRecordBuffer;
    if (std::memcmp(record, "FILE", 4) != 0) {
        return false;
    }
    const auto first_attr = *reinterpret_cast<std::uint16_t*>(record + 0x14);
    auto* attr = record + first_attr;
    while (attr < record + output->FileRecordLength) {
        const auto attr_type = *reinterpret_cast<std::uint32_t*>(attr);
        if (attr_type == 0xFFFFFFFFU) {
            break;
        }
        const auto attr_len = *reinterpret_cast<std::uint32_t*>(attr + 4);
        if (attr_len == 0) {
            break;
        }
        if (attr_type == 0x80) {
            const auto non_resident = *(attr + 0x08);
            const auto name_len = *(attr + 0x09);
            if (name_len == 0 && non_resident != 0) {
                const auto run_offset = *reinterpret_cast<std::uint16_t*>(attr + 0x20);
                auto* run = attr + run_offset;
                std::uint64_t current_lcn = 0;
                std::uint64_t current_vcn = 0;
                while (*run != 0) {
                    const auto header = *run++;
                    const auto len_bytes = header & 0x0F;
                    const auto off_bytes = (header >> 4) & 0x0F;
                    std::uint64_t run_length = 0;
                    for (int i = 0; i < len_bytes; ++i) {
                        run_length |= static_cast<std::uint64_t>(*run++) << (i * 8);
                    }
                    std::int64_t run_offset_val = 0;
                    for (int i = 0; i < off_bytes; ++i) {
                        run_offset_val |= static_cast<std::int64_t>(*run++) << (i * 8);
                    }
                    if (off_bytes > 0 &&
                        (run_offset_val & (1LL << (off_bytes * 8 - 1))) != 0) {
                        for (int i = off_bytes; i < 8; ++i) {
                            run_offset_val |= static_cast<std::int64_t>(0xFF) << (i * 8);
                        }
                    }
                    current_lcn = static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(current_lcn) + run_offset_val);
                    if (current_lcn != 0 && run_length > 0) {
                        extents.push_back(FileExtent{current_lcn, run_length});
                    }
                    current_vcn += run_length;
                    (void)current_vcn;
                }
                return !extents.empty();
            }
            if (name_len == 0 && non_resident == 0) {
                return true; // resident tiny file
            }
        }
        attr += attr_len;
    }
    return false;
}

[[nodiscard]] bool query_file_extents(const std::wstring& file_path,
                                      std::vector<FileExtent>& extents) {
    extents.clear();
    auto handle = open_file_for_extents(file_path);
    if (handle.valid() && query_file_extents_via_handle(handle.get(), extents)) {
        return true;
    }
    // Sharing violation / access denied on pagefile — fall back to MFT (old product path).
    return query_file_extents_via_mft(file_path, extents);
}

void merge_ranges(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) {
    if (ranges.size() < 2) {
        return;
    }
    std::ranges::sort(ranges, {}, &std::pair<std::uint64_t, std::uint64_t>::first);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
    merged.reserve(ranges.size());
    for (const auto& range : ranges) {
        if (range.second <= range.first) {
            continue;
        }
        if (!merged.empty() && range.first <= merged.back().second) {
            merged.back().second = (std::max)(merged.back().second, range.second);
        } else {
            merged.push_back(range);
        }
    }
    ranges = std::move(merged);
}

[[nodiscard]] std::uint64_t range_total_bytes(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) noexcept {
    std::uint64_t total = 0;
    for (const auto& range : ranges) {
        if (range.second > range.first) {
            total += range.second - range.first;
        }
    }
    return total;
}

[[nodiscard]] bool try_query_file_extents_on_root(const std::wstring& root_with_slash,
                                                  const wchar_t* file_name,
                                                  std::vector<FileExtent>& extents) {
    return query_file_extents(root_with_slash + file_name, extents) && !extents.empty();
}

// Merge one junk file's LCN runs into free_ranges (AipCopy AddFileToExcludedClusters).
[[nodiscard]] std::uint64_t
append_file_extents_to_plan(FreeSkipPlan& plan, const std::vector<FileExtent>& extents,
                            const std::uint64_t cluster_size) {
    std::uint64_t covered = 0;
    for (const auto& extent : extents) {
        if (extent.lcn > (std::numeric_limits<std::uint64_t>::max)() / cluster_size) {
            continue;
        }
        const auto start = extent.lcn * cluster_size;
        auto end = start + extent.length_clusters * cluster_size;
        if (start >= plan.total_bytes) {
            continue;
        }
        if (end > plan.total_bytes) {
            end = plan.total_bytes;
        }
        if (end > start) {
            covered += end - start;
            append_free_range(plan.free_ranges, start, end);
        }
    }
    return covered;
}

std::uint64_t merge_page_and_hibernation_exclusions(FreeSkipPlan& plan,
                                                    const std::filesystem::path& read_device_path,
                                                    const std::uint32_t cluster_size_bytes) {
    if (read_device_path.empty() || plan.total_bytes == 0) {
        return 0;
    }
    auto cluster_size = static_cast<std::uint64_t>(cluster_size_bytes);
    if (cluster_size == 0) {
        cluster_size = 4096;
    }

    // AipCopy: open pagefile/hiberfil on the same static volume root used for the volume read
    // (live Volume GUID or VSS snapshot device), then clear those LCNs in the free bitmap.
    const auto normalized = normalize_volume_root_for_files(read_device_path);
    if (normalized.empty()) {
        return 0;
    }
    auto primary_root = trailing_slash_path(std::filesystem::path(normalized));

    // Optional temporary DOS map for VSS GLOBALROOT when direct file open fails (AipCopy
    // DefineDosDevice(DDD_RAW_TARGET_PATH) pattern). Kept for the whole merge so all three
    // names share one mapping.
    TemporaryDosDeviceMap dos_map;
    std::wstring dos_root;
    const auto maybe_nt = nt_device_path_for_dos_map(normalized);

    static constexpr const wchar_t* kNames[] = {L"pagefile.sys", L"hiberfil.sys", L"swapfile.sys"};
    const auto before_bytes = range_total_bytes(plan.free_ranges);
    std::uint64_t added_reported = 0;
    for (const auto* name : kNames) {
        std::vector<FileExtent> extents;
        if (!try_query_file_extents_on_root(primary_root, name, extents)) {
            if (dos_root.empty() && maybe_nt) {
                dos_map = TemporaryDosDeviceMap::try_map(*maybe_nt);
                if (dos_map.active()) {
                    dos_root = dos_map.win32_root_with_slash();
                }
            }
            if (dos_root.empty() || !try_query_file_extents_on_root(dos_root, name, extents)) {
                continue;
            }
        }
        added_reported += append_file_extents_to_plan(plan, extents, cluster_size);
    }
    merge_ranges(plan.free_ranges);
    plan.free_bytes = range_total_bytes(plan.free_ranges);
    if (!plan.free_ranges.empty()) {
        plan.applied = true;
    }
    // Report bytes newly covered by exclusions (best-effort; overlaps with free map inflate less).
    const auto after_bytes = plan.free_bytes;
    if (after_bytes > before_bytes) {
        return after_bytes - before_bytes;
    }
    return added_reported;
}

FreeSkipPlan build_free_skip_plan(const std::filesystem::path& device_path,
                                  const std::string_view filesystem,
                                  const std::uint64_t total_size_bytes,
                                  const std::uint32_t cluster_size_bytes) {
    FreeSkipPlan empty;
    empty.total_bytes = total_size_bytes;
    empty.filesystem = normalize_filesystem(filesystem);
    if (device_path.empty() || total_size_bytes == 0) {
        return empty;
    }
    const auto kind = free_skip_kind(filesystem);
    if (kind == FreeSkipKind::kNone) {
        return empty;
    }

    const auto open_path = device_open_path(device_path);
    std::uint64_t hint_cluster = cluster_size_bytes;
    std::uint64_t hint_clusters = 0;
    std::optional<FatGeometry> fat_geometry;
    if (kind == FreeSkipKind::kFatRemapped) {
        fat_geometry = read_fat_geometry(open_path, total_size_bytes);
        if (!fat_geometry) {
            return empty;
        }
        hint_cluster = fat_geometry->cluster_size_bytes;
        hint_clusters = fat_geometry->hint_total_clusters;
    } else if (hint_cluster > 0) {
        hint_clusters = total_size_bytes / hint_cluster;
    }

    auto bitmap = query_volume_bitmap(open_path, hint_cluster, hint_clusters);
    if (!bitmap) {
        return empty;
    }
    if (kind == FreeSkipKind::kFatRemapped && fat_geometry &&
        fat_geometry->cluster_size_bytes != 0 &&
        fat_geometry->cluster_size_bytes != bitmap->cluster_size_bytes) {
        // Prefer BPB cluster size when GetDiskFreeSpace disagrees (old product behavior).
        bitmap->cluster_size_bytes = fat_geometry->cluster_size_bytes;
    }

    if (kind == FreeSkipKind::kFatRemapped) {
        return build_fat_plan(*bitmap, *fat_geometry, total_size_bytes);
    }
    auto plan = build_ntfs_plan(*bitmap, total_size_bytes);
    plan.filesystem = normalize_filesystem(filesystem);
    return plan;
}

struct FreeSkipBlockSource::Impl final {
    std::unique_ptr<ports::IBlockSource> inner;
    FreeSkipPlan plan;
};

FreeSkipBlockSource::FreeSkipBlockSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FreeSkipBlockSource::~FreeSkipBlockSource() = default;

base::Result<std::unique_ptr<FreeSkipBlockSource>>
FreeSkipBlockSource::wrap(std::unique_ptr<ports::IBlockSource> inner, FreeSkipPlan plan) {
    if (!inner) {
        return base::Result<std::unique_ptr<FreeSkipBlockSource>>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "free-skip source requires an inner block source",
        });
    }
    if (!plan.applied || plan.free_ranges.empty()) {
        return base::Result<std::unique_ptr<FreeSkipBlockSource>>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "free-skip plan is empty",
        });
    }
    // Logical size always follows the inner device/source (inventory/metadata length).
    plan.total_bytes = inner->size_bytes();
    auto impl = std::make_unique<Impl>();
    impl->inner = std::move(inner);
    impl->plan = std::move(plan);
    return base::Result<std::unique_ptr<FreeSkipBlockSource>>::success(
        std::unique_ptr<FreeSkipBlockSource>(new FreeSkipBlockSource(std::move(impl))));
}

std::uint64_t FreeSkipBlockSource::size_bytes() const noexcept {
    return impl_->inner->size_bytes();
}

const FreeSkipPlan& FreeSkipBlockSource::plan() const noexcept { return impl_->plan; }

base::Result<std::size_t> FreeSkipBlockSource::read(const std::uint64_t offset,
                                                    const std::span<std::byte> destination,
                                                    const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::size_t>::failure(
            base::Error{base::ErrorCode::kCancelled, "block read cancelled"});
    }
    const auto size = impl_->inner->size_bytes();
    if (offset > size) {
        return base::Result<std::size_t>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "read offset is out of range"});
    }
    if (destination.empty() || offset == size) {
        return base::Result<std::size_t>::success(0);
    }
    const auto request =
        (std::min)(static_cast<std::uint64_t>(destination.size()), size - offset);
    std::size_t filled = 0;
    while (filled < static_cast<std::size_t>(request)) {
        if (cancellation.stop_requested()) {
            return base::Result<std::size_t>::failure(
                base::Error{base::ErrorCode::kCancelled, "block read cancelled"});
        }
        const auto position = offset + filled;
        const auto remaining = request - static_cast<std::uint64_t>(filled);
        const auto free_len = free_length_at(impl_->plan.free_ranges, position, remaining);
        if (free_len > 0) {
            const auto count = static_cast<std::size_t>(free_len);
            std::fill(destination.begin() + static_cast<std::ptrdiff_t>(filled),
                      destination.begin() + static_cast<std::ptrdiff_t>(filled + count),
                      std::byte{0});
            filled += count;
            continue;
        }
        const auto used_len = used_length_at(impl_->plan.free_ranges, position, remaining);
        if (used_len == 0) {
            return base::Result<std::size_t>::failure(base::Error{
                base::ErrorCode::kInternal,
                "free-skip map produced an empty used range",
            });
        }
        const auto count = static_cast<std::size_t>(used_len);
        auto read_result = read_used_range(*impl_->inner, position, destination.subspan(filled, count),
                                           cancellation);
        if (!read_result) {
            return read_result;
        }
        filled += count;
    }
    return base::Result<std::size_t>::success(static_cast<std::size_t>(request));
}

} // namespace aegra::adapters::windows_disk
