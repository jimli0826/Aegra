#pragma once

#include "aegra/format/manifest.h"

#include <cstdint>
#include <set>

namespace aegra::adapters::dokan::detail {

[[nodiscard]] bool is_mountable_partition(const format::Partition& part);

// Partition numbers on the given source disk that should receive drive letters.
[[nodiscard]] std::set<std::uint32_t>
find_data_partition_numbers(const format::Manifest& manifest,
                            std::uint32_t source_disk_number);

// True if the manifest contains a disk with the given number.
[[nodiscard]] bool manifest_has_disk(const format::Manifest& manifest,
                                     std::uint32_t source_disk_number);

} // namespace aegra::adapters::dokan::detail
