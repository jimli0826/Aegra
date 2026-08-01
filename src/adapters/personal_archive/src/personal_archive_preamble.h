#pragma once

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/format/manifest.h"
#include "aegra/format/personal_archive.h"

#include <cstdint>
#include <fstream>

namespace aegra::adapters::personal_archive::detail {

struct ParsedPreamble final {
    format::personal_archive::BackupHeader header;
    format::Manifest manifest;
};

[[nodiscard]] base::Result<ParsedPreamble> read_archive_preamble(std::ifstream& input,
                                                                 const ArchiveOpenRequest& request,
                                                                 std::uint64_t file_size);

} // namespace aegra::adapters::personal_archive::detail
