#pragma once

#include "personal_archive_chunk_builder.h"

#include "aegra/adapters/crypto_sodium/payload_crypto.h"
#include "aegra/base/result.h"
#include "aegra/format/personal_archive.h"

#include <span>
#include <vector>

namespace aegra::adapters::personal_archive::detail {

[[nodiscard]] base::Result<void>
protect_archive_chunk(PreparedArchiveChunk& chunk,
                      const crypto_sodium::PayloadCipher& payload_cipher);

[[nodiscard]] base::Result<std::vector<std::byte>>
unprotect_archive_chunk(const format::personal_archive::ChunkHeader& header,
                        std::span<const format::personal_archive::BlockEntry> entries,
                        std::span<const std::byte> ciphertext,
                        const crypto_sodium::PayloadCipher* payload_cipher);

} // namespace aegra::adapters::personal_archive::detail
