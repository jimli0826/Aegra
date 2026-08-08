#include "personal_archive_payload.h"

#include <utility>

namespace aegra::adapters::personal_archive::detail {
namespace {

namespace archive = format::personal_archive;

[[nodiscard]] base::Result<std::vector<std::byte>>
make_authenticated_data(const archive::EncodedBackupHeader& part_header,
                        const std::uint64_t body_size, const archive::ChunkHeader& header,
                        const std::span<const archive::BlockEntry> entries) {
    const auto prefix = archive::make_volume_chunk_record_prefix(body_size);
    auto encoded_prefix = archive::encode_archive_record_prefix(prefix);
    if (!encoded_prefix) {
        return base::Result<std::vector<std::byte>>::failure(encoded_prefix.error());
    }
    auto authenticated_header = header;
    authenticated_header.payload_authentication_tag.fill(std::byte{0});
    auto encoded_header = archive::encode_chunk_header(authenticated_header);
    if (!encoded_header) {
        return base::Result<std::vector<std::byte>>::failure(encoded_header.error());
    }
    std::vector<std::byte> result;
    result.reserve(part_header.size() + encoded_prefix.value().size() +
                   encoded_header.value().size() + entries.size() * archive::kBlockEntrySize);
    result.insert(result.end(), part_header.begin(), part_header.end());
    result.insert(result.end(), encoded_prefix.value().begin(), encoded_prefix.value().end());
    result.insert(result.end(), encoded_header.value().begin(), encoded_header.value().end());
    for (const auto& entry : entries) {
        auto encoded_entry = archive::encode_block_entry(entry);
        if (!encoded_entry) {
            return base::Result<std::vector<std::byte>>::failure(encoded_entry.error());
        }
        result.insert(result.end(), encoded_entry.value().begin(), encoded_entry.value().end());
    }
    return base::Result<std::vector<std::byte>>::success(std::move(result));
}

[[nodiscard]] std::uint64_t chunk_body_size(const archive::ChunkHeader& header) noexcept {
    return static_cast<std::uint64_t>(header.block_entry_count) * archive::kBlockEntrySize +
           header.payload_size;
}

} // namespace

base::Result<void> protect_archive_chunk(PreparedArchiveChunk& chunk,
                                         const archive::EncodedBackupHeader& part_header,
                                         const crypto_sodium::PayloadCipher& payload_cipher) {
    auto nonce = crypto_sodium::create_payload_nonce();
    if (!nonce) {
        return base::Result<void>::failure(nonce.error());
    }
    chunk.header.payload_nonce = nonce.value();
    auto authenticated_data =
        make_authenticated_data(part_header, chunk_body_size(chunk.header), chunk.header,
                                chunk.entries);
    if (!authenticated_data) {
        return base::Result<void>::failure(authenticated_data.error());
    }
    auto protected_payload =
        payload_cipher.protect(chunk.payload, authenticated_data.value(), nonce.value());
    if (!protected_payload) {
        return base::Result<void>::failure(protected_payload.error());
    }
    auto protected_value = std::move(protected_payload).value();
    chunk.payload = std::move(protected_value.ciphertext);
    chunk.header.payload_authentication_tag = protected_value.tag;
    return base::Result<void>::success();
}

base::Result<std::vector<std::byte>>
unprotect_archive_chunk(const archive::EncodedBackupHeader& part_header,
                        const archive::ChunkHeader& header,
                        const std::span<const archive::BlockEntry> entries,
                        const std::span<const std::byte> ciphertext,
                        const crypto_sodium::PayloadCipher* payload_cipher) {
    if (payload_cipher == nullptr) {
        return base::Result<std::vector<std::byte>>::success(
            std::vector<std::byte>(ciphertext.begin(), ciphertext.end()));
    }
    auto authenticated_data =
        make_authenticated_data(part_header, chunk_body_size(header), header, entries);
    if (!authenticated_data) {
        return base::Result<std::vector<std::byte>>::failure(authenticated_data.error());
    }
    return payload_cipher->unprotect(ciphertext, authenticated_data.value(), header.payload_nonce,
                                     header.payload_authentication_tag);
}

} // namespace aegra::adapters::personal_archive::detail
