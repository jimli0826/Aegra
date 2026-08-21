#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_core/types.h"
#include "aegra/ntfs_resize/ntfs_shrink_analyzer.h"
#include "aegra/ports/random_access.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aegra::ntfs_resize::detail {

inline constexpr std::size_t kMaxAttributeListExtensions = 32;
inline constexpr std::uint64_t kMaxBitmapLoadBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kAttrVolumeInformation = 0x70;
inline constexpr std::uint16_t kVolumeFlagDirty = 0x0001;
inline constexpr std::uint32_t kFileNumberMft = 0;
inline constexpr std::uint32_t kFileNumberLogFile = 2;
inline constexpr std::uint32_t kFileNumberVolume = 3;
inline constexpr std::uint32_t kFileNumberBitmap = 6;
inline constexpr std::uint32_t kFileNumberBoot = 7;

struct NtfsVolumeView final {
    ports::IRandomAccessReader* reader{nullptr};
    ntfs_core::BootGeometry geometry{};
    ntfs_core::AttributeValue mft_data{};
    std::vector<std::byte> boot_sector_bytes{};
    INtfsShrinkAnalysisObserver* observer{nullptr};
    std::uint64_t target_capacity_bytes{0};
    bool mft_ready{false};
};

struct NtfsVolumeOpenDiagnostics final {
    INtfsShrinkAnalysisObserver* observer{nullptr};
    std::uint64_t target_capacity_bytes{0};
    ports::BlockDeviceGeometry target_geometry{};
};

[[nodiscard]] base::Result<std::vector<std::byte>>
read_exact_bytes(ports::IRandomAccessReader& reader, std::uint64_t offset, std::size_t size,
                 base::CancellationToken cancellation);

[[nodiscard]] base::Result<NtfsVolumeView>
open_ntfs_volume_view(ports::IRandomAccessReader& reader,
                      std::uint64_t expected_source_logical_size_bytes,
                      base::CancellationToken cancellation,
                      const NtfsVolumeOpenDiagnostics& diagnostics = {});

[[nodiscard]] base::Result<ntfs_core::ParsedMftRecord>
read_mft_record(NtfsVolumeView& view, std::uint64_t record_number,
                base::CancellationToken cancellation);

[[nodiscard]] base::Result<std::vector<std::uint64_t>>
collect_extension_record_numbers(NtfsVolumeView& view, const ntfs_core::ParsedMftRecord& base,
                                 base::CancellationToken cancellation);

[[nodiscard]] base::Result<ntfs_core::AttributeValue>
load_unnamed_data_attribute(NtfsVolumeView& view, std::uint64_t record_number,
                            base::CancellationToken cancellation);

[[nodiscard]] base::Result<ntfs_core::AttributeValue>
load_unnamed_attribute(NtfsVolumeView& view, std::uint64_t record_number,
                       std::uint32_t attribute_type, base::CancellationToken cancellation);

[[nodiscard]] base::Result<std::vector<std::byte>>
read_attribute_payload(NtfsVolumeView& view, const ntfs_core::AttributeValue& attribute,
                       std::uint64_t maximum_bytes, base::CancellationToken cancellation);

} // namespace aegra::ntfs_resize::detail
