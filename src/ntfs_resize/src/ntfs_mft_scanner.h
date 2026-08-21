#pragma once

#include "ntfs_volume_view.h"

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::ntfs_resize::detail {

struct OutboundExtent final {
    std::uint64_t mft_record_number{0};
    std::uint32_t attribute_type{0};
    std::u16string attribute_name;
    std::uint16_t attribute_id{0};
    bool attribute_compressed{false};
    bool attribute_encrypted{false};
    ntfs_core::DataRun run{};
};

struct OwnedAttributeRuns final {
    std::uint64_t mft_record_number{0};
    std::uint16_t record_sequence{0};
    std::uint32_t attribute_type{0};
    std::u16string attribute_name;
    std::uint16_t attribute_id{0};
    bool compressed{false};
    bool encrypted{false};
    std::uint32_t attribute_record_offset{0};
    std::uint32_t attribute_length{0};
    std::uint16_t runlist_offset{0};
    std::uint32_t runlist_capacity_bytes{0};
    std::vector<ntfs_core::DataRun> runs;
};

struct MftScanResult final {
    std::vector<OutboundExtent> outbound_extents;
    std::vector<OwnedAttributeRuns> attributes_with_outbound;
    std::uint64_t mft_record_count{0};
    std::uint64_t allocated_beyond_clusters{0};
};

[[nodiscard]] base::Result<void>
reject_dirty_or_unsafe_logfile(NtfsVolumeView& view, base::CancellationToken cancellation);

[[nodiscard]] base::Result<MftScanResult>
scan_outbound_mft_extents(NtfsVolumeView& view, std::uint64_t new_total_cluster_count,
                          base::CancellationToken cancellation);

} // namespace aegra::ntfs_resize::detail
