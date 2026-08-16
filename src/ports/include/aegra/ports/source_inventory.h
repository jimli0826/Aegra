#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::ports {

// Platform inventory record. source_id is stable and opaque to Desktop; stable_key is the trusted
// physical identity consumed only inside Service/Worker composition.
struct SourceInventoryRecord final {
    std::string source_id;
    std::string stable_key;
    std::string display_name;
    contracts::SourceKind kind{contracts::SourceKind::kVolume};
    contracts::SourceAvailability availability{contracts::SourceAvailability::kUnavailable};
    std::uint64_t capacity_bytes{0};
    // Free bytes on the volume when known (GetDiskFreeSpaceEx); 0 if unknown/unmounted.
    std::uint64_t free_bytes{0};
    std::uint64_t disk_capacity_bytes{0};
    bool is_system{false};
    bool is_read_only{false};
    std::uint32_t disk_number{0};
    // Primary extent start on the physical disk (bytes). 0 when unknown or disk shell.
    std::uint64_t offset_bytes{0};
    std::string mount_letter;
    std::string volume_label;
    std::string health_status;
    std::string partition_style;
    // Physical media hint for Home charts (SSD/HDD/USB/Virtual/Unknown).
    std::string media_type;
};

class ISourceInventory {
  public:
    ISourceInventory() = default;
    virtual ~ISourceInventory() = default;
    ISourceInventory(const ISourceInventory&) = delete;
    ISourceInventory& operator=(const ISourceInventory&) = delete;
    ISourceInventory(ISourceInventory&&) = delete;
    ISourceInventory& operator=(ISourceInventory&&) = delete;

    // Returns a stable snapshot with unique source_id values, ordered by source_id ascending.
    [[nodiscard]] virtual base::Result<std::vector<SourceInventoryRecord>>
    list_sources(base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
