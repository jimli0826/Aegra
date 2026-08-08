#pragma once

#include "aegra/adapters/crypto_sodium/payload_crypto.h"
#include "aegra/base/result.h"
#include "aegra/format/file_index.h"
#include "aegra/format/personal_archive.h"

#include <cstdint>
#include <fstream>
#include <string_view>
#include <vector>

namespace aegra::adapters::personal_archive::secondary_index {

struct TreeWriteResult final {
    format::personal_archive::IndexRootLocator locator;
    std::uint64_t page_count{0};
};

/// Writes Entry ID secondary B+tree and returns root locator + page count.
[[nodiscard]] base::Result<TreeWriteResult>
write_entry_id_index(std::ofstream& output, std::uint64_t& next_page_id,
                     bool encryption_enabled, crypto_sodium::PayloadCipher* index_cipher,
                     const format::personal_archive::EncodedBackupHeader& part_header,
                     std::vector<format::file_index::EntryIdIndexRecord> records);

[[nodiscard]] base::Result<TreeWriteResult>
write_stream_index(std::ofstream& output, std::uint64_t& next_page_id, bool encryption_enabled,
                   crypto_sodium::PayloadCipher* index_cipher,
                   const format::personal_archive::EncodedBackupHeader& part_header,
                   std::vector<format::file_index::StreamIndexRecord> records);

[[nodiscard]] base::Result<TreeWriteResult>
write_chunk_index(std::ofstream& output, std::uint64_t& next_page_id, bool encryption_enabled,
                  crypto_sodium::PayloadCipher* index_cipher,
                  const format::personal_archive::EncodedBackupHeader& part_header,
                  std::vector<format::file_index::ChunkIndexRecord> records);

} // namespace aegra::adapters::personal_archive::secondary_index
