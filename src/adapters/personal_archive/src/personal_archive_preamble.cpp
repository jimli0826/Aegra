#include "personal_archive_preamble.h"

#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/base/error.h"
#include "aegra/format/manifest_codec.h"

#include <algorithm>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive::detail {
namespace {

namespace archive = format::personal_archive;

struct EncodedMetadata final {
    archive::BackupHeader header;
    std::vector<std::byte> header_bytes;
    archive::MetadataEnvelopeHeader envelope;
    std::vector<std::byte> envelope_bytes;
    std::vector<std::byte> payload;
};

[[nodiscard]] base::Error corrupt(std::string message) {
    return {base::ErrorCode::kCorruptData, std::move(message)};
}

[[nodiscard]] char* as_chars(std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) stream byte-buffer boundary.
    return reinterpret_cast<char*>(value);
}

[[nodiscard]] base::Result<std::vector<std::byte>>
read_exact(std::ifstream& input, const std::uint64_t offset, const std::size_t size) {
    if (size > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) {
        return base::Result<std::vector<std::byte>>::failure(
            corrupt("archive metadata read exceeds stream limit"));
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::vector<std::byte> result(size);
    input.read(as_chars(result.data()), static_cast<std::streamsize>(size));
    if (!input || input.gcount() != static_cast<std::streamsize>(size)) {
        return base::Result<std::vector<std::byte>>::failure(
            {base::ErrorCode::kIoFailure, "personal archive metadata is truncated"});
    }
    return base::Result<std::vector<std::byte>>::success(std::move(result));
}

[[nodiscard]] std::vector<std::byte> make_authenticated_data(std::span<const std::byte> header,
                                                             std::span<const std::byte> envelope) {
    std::vector<std::byte> result;
    result.reserve(header.size() + envelope.size());
    result.insert(result.end(), header.begin(), header.end());
    result.insert(result.end(), envelope.begin(), envelope.end());
    return result;
}

[[nodiscard]] crypto_sodium::ProtectedMetadata
make_protected_metadata(const archive::MetadataEnvelopeHeader& envelope,
                        std::span<const std::byte> ciphertext, std::span<const std::byte> tag) {
    crypto_sodium::ProtectedMetadata result;
    result.kdf = {envelope.kdf_opslimit, envelope.kdf_memlimit_bytes,
                  envelope.kdf_parameters_version};
    result.salt = envelope.salt;
    result.nonce = envelope.nonce;
    result.ciphertext.assign(ciphertext.begin(), ciphertext.end());
    std::copy(tag.begin(), tag.end(), result.tag.begin());
    return result;
}

[[nodiscard]] base::Result<EncodedMetadata> read_encoded_metadata(std::ifstream& input,
                                                                  const ArchiveOpenRequest& request,
                                                                  const std::uint64_t file_size) {
    auto header_bytes = read_exact(input, 0, archive::kBackupHeaderSize);
    if (!header_bytes) {
        return base::Result<EncodedMetadata>::failure(header_bytes.error());
    }
    auto header = archive::decode_backup_header(header_bytes.value());
    if (!header) {
        return base::Result<EncodedMetadata>::failure(header.error());
    }
    const auto overhead = archive::kMetadataEnvelopeHeaderSize + crypto_sodium::kMetadataTagSize;
    const bool exceeds_limit = header.value().cbor_size > overhead &&
                               header.value().cbor_size - overhead > request.maximum_metadata_size;
    if (exceeds_limit || header.value().first_chunk_offset > file_size) {
        return base::Result<EncodedMetadata>::failure(
            corrupt("archive metadata range exceeds limits"));
    }
    auto envelope_bytes =
        read_exact(input, header.value().cbor_offset, archive::kMetadataEnvelopeHeaderSize);
    if (!envelope_bytes) {
        return base::Result<EncodedMetadata>::failure(envelope_bytes.error());
    }
    auto envelope = archive::decode_metadata_envelope_header(envelope_bytes.value());
    if (!envelope) {
        return base::Result<EncodedMetadata>::failure(envelope.error());
    }
    if (envelope.value().ciphertext_size > request.maximum_metadata_size) {
        return base::Result<EncodedMetadata>::failure(
            corrupt("archive metadata exceeds configured limit"));
    }
    const auto expected_size = archive::kMetadataEnvelopeHeaderSize +
                               envelope.value().ciphertext_size + envelope.value().tag_size;
    if (expected_size != header.value().cbor_size) {
        return base::Result<EncodedMetadata>::failure(
            corrupt("archive metadata envelope size is invalid"));
    }
    auto payload = read_exact(
        input, header.value().cbor_offset + archive::kMetadataEnvelopeHeaderSize,
        static_cast<std::size_t>(envelope.value().ciphertext_size + envelope.value().tag_size));
    if (!payload) {
        return base::Result<EncodedMetadata>::failure(payload.error());
    }
    return base::Result<EncodedMetadata>::success(
        {header.value(), std::move(header_bytes).value(), envelope.value(),
         std::move(envelope_bytes).value(), std::move(payload).value()});
}

[[nodiscard]] base::Result<format::Manifest> decrypt_manifest(const EncodedMetadata& encoded,
                                                              const std::string_view password) {
    const auto ciphertext = std::span<const std::byte>(encoded.payload)
                                .first(static_cast<std::size_t>(encoded.envelope.ciphertext_size));
    const auto tag = std::span<const std::byte>(encoded.payload).last(encoded.envelope.tag_size);
    auto protected_metadata = make_protected_metadata(encoded.envelope, ciphertext, tag);
    const auto aad = make_authenticated_data(encoded.header_bytes, encoded.envelope_bytes);
    auto plaintext = crypto_sodium::unprotect_metadata(protected_metadata, password, aad);
    if (!plaintext) {
        return base::Result<format::Manifest>::failure(plaintext.error());
    }
    return format::decode_manifest_cbor(plaintext.value());
}

[[nodiscard]] std::uint32_t backup_type_flag(const format::BackupType type) noexcept {
    switch (type) {
    case format::BackupType::kFull:
        return archive::kBackupFlagFull;
    case format::BackupType::kIncremental:
        return archive::kBackupFlagIncremental;
    case format::BackupType::kDifferential:
        return archive::kBackupFlagDifferential;
    }
    return 0;
}

[[nodiscard]] base::Result<void> validate_manifest_binding(const EncodedMetadata& encoded,
                                                           const format::Manifest& manifest) {
    constexpr auto type_mask = archive::kBackupFlagFull | archive::kBackupFlagIncremental |
                               archive::kBackupFlagDifferential;
    if ((encoded.header.flags & type_mask) != backup_type_flag(manifest.backup_job.backup_type)) {
        return base::Result<void>::failure(
            corrupt("archive header and manifest backup types do not match"));
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<ParsedPreamble> read_archive_preamble(std::ifstream& input,
                                                   const ArchiveOpenRequest& request,
                                                   const std::uint64_t file_size) {
    auto encoded = read_encoded_metadata(input, request, file_size);
    if (!encoded) {
        return base::Result<ParsedPreamble>::failure(encoded.error());
    }
    auto manifest = decrypt_manifest(encoded.value(), request.password);
    if (!manifest) {
        return base::Result<ParsedPreamble>::failure(manifest.error());
    }
    auto binding = validate_manifest_binding(encoded.value(), manifest.value());
    if (!binding) {
        return base::Result<ParsedPreamble>::failure(binding.error());
    }
    return base::Result<ParsedPreamble>::success(
        {encoded.value().header, std::move(manifest).value()});
}

} // namespace aegra::adapters::personal_archive::detail
