#pragma once

#include "aegra/base/result.h"
#include "aegra/ntfs_core/types.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::ntfs_core {

[[nodiscard]] base::Result<ParsedMftRecord>
parse_mft_record_bytes(std::span<std::byte> record_bytes, std::uint32_t bytes_per_sector,
                       std::uint64_t expected_record_number);

} // namespace aegra::ntfs_core
