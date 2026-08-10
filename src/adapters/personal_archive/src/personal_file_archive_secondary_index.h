#pragma once

#include "aegra/adapters/crypto_sodium/payload_crypto.h"
#include "aegra/base/result.h"
#include "aegra/format/file_index.h"
#include "aegra/format/personal_archive.h"
#include "win32_output_file.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace aegra::adapters::personal_archive::secondary_index {

struct TreeWriteResult final {
    format::personal_archive::IndexRootLocator locator;
    std::uint64_t page_count{0};
};

/// Writes Entry ID secondary B+tree and returns root locator + page count.
[[nodiscard]] base::Result<TreeWriteResult>
write_entry_id_index(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
                     bool encryption_enabled, crypto_sodium::PayloadCipher* index_cipher,
                     const format::personal_archive::EncodedBackupHeader& part_header,
                     std::vector<format::file_index::EntryIdIndexRecord> records);

[[nodiscard]] base::Result<TreeWriteResult>
write_stream_index(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
                   bool encryption_enabled,
                   crypto_sodium::PayloadCipher* index_cipher,
                   const format::personal_archive::EncodedBackupHeader& part_header,
                   std::vector<format::file_index::StreamIndexRecord> records);

[[nodiscard]] base::Result<TreeWriteResult>
write_chunk_index(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
                  bool encryption_enabled,
                  crypto_sodium::PayloadCipher* index_cipher,
                  const format::personal_archive::EncodedBackupHeader& part_header,
                  std::vector<format::file_index::ChunkIndexRecord> records);

} // namespace aegra::adapters::personal_archive::secondary_index
