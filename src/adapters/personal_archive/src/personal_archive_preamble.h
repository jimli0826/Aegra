#pragma once

#include "win32_input_file.h"

#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/format/manifest.h"
#include "aegra/format/personal_archive.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aegra::adapters::personal_archive::detail {

struct ParsedPreamble final {
    format::personal_archive::BackupHeader header;
    format::Manifest manifest;
    crypto_sodium::KdfParameters kdf;
    std::array<std::byte, crypto_sodium::kMetadataSaltSize> salt{};
};

[[nodiscard]] format::BackupType
archive_backup_type(const format::personal_archive::BackupHeader& header) noexcept;

[[nodiscard]] base::Result<ParsedPreamble> read_archive_preamble(Win32InputFile& input,
                                                                 const ArchiveOpenRequest& request,
                                                                 std::uint64_t file_size);

} // namespace aegra::adapters::personal_archive::detail
