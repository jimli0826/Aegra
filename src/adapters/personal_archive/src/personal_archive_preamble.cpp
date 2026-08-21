#include "personal_archive_preamble.h"

#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/base/error.h"
#include "aegra/format/manifest_codec.h"

#include <algorithm>
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

[[nodiscard]] base::Result<std::vector<std::byte>>
read_exact(Win32InputFile& input, const std::uint64_t offset, const std::size_t size) {
    auto bytes = input.read_exact_at(offset, size);
    if (!bytes) {
        return base::Result<std::vector<std::byte>>::failure(
            {base::ErrorCode::kIoFailure, "personal archive metadata is truncated"});
    }
    return bytes;
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

[[nodiscard]] base::Result<EncodedMetadata> read_encoded_metadata(Win32InputFile& input,
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
    if (exceeds_limit || header.value().first_record_offset > file_size) {
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
    const bool encrypted =
        (encoded.envelope.flags & archive::kCborMetadataFlagEncrypted) != 0;
    if (!encrypted) {
        if (!password.empty()) {
            return base::Result<format::Manifest>::failure(
                corrupt("unencrypted archive opened with a password"));
        }
        return format::decode_manifest_cbor(ciphertext);
    }
    // Empty password on encrypted metadata is an authorization problem for Shell/UI, not a
    // caller-parameter bug — keep ErrorCode stable as kUnauthorized (shell.password_required).
    if (password.empty()) {
        return base::Result<format::Manifest>::failure(
            {base::ErrorCode::kUnauthorized, "shell.password_required"});
    }
    if (encoded.envelope.tag_size == 0 ||
        encoded.payload.size() <
            static_cast<std::size_t>(encoded.envelope.ciphertext_size + encoded.envelope.tag_size)) {
        return base::Result<format::Manifest>::failure(
            corrupt("encrypted metadata payload is truncated"));
    }
    const auto tag = std::span<const std::byte>(encoded.payload).last(encoded.envelope.tag_size);
    auto protected_metadata = make_protected_metadata(encoded.envelope, ciphertext, tag);
    const auto aad = make_authenticated_data(encoded.header_bytes, encoded.envelope_bytes);
    auto plaintext = crypto_sodium::unprotect_metadata(protected_metadata, password, aad);
    if (!plaintext) {
        if (plaintext.error().code != base::ErrorCode::kUnauthorized) {
            return base::Result<format::Manifest>::failure(plaintext.error());
        }
        // Wrong password / AEAD failure — stable code for Shell (not localized text matching).
        return base::Result<format::Manifest>::failure(
            {base::ErrorCode::kUnauthorized, "shell.password_invalid"});
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

format::BackupType archive_backup_type(const archive::BackupHeader& header) noexcept {
    if ((header.flags & archive::kBackupFlagIncremental) != 0) {
        return format::BackupType::kIncremental;
    }
    if ((header.flags & archive::kBackupFlagDifferential) != 0) {
        return format::BackupType::kDifferential;
    }
    return format::BackupType::kFull;
}

base::Result<ParsedPreamble> read_archive_preamble(Win32InputFile& input,
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
        {encoded.value().header,
         std::move(manifest).value(),
         {encoded.value().envelope.kdf_opslimit, encoded.value().envelope.kdf_memlimit_bytes,
          encoded.value().envelope.kdf_parameters_version},
         encoded.value().envelope.salt});
}

} // namespace aegra::adapters::personal_archive::detail

namespace aegra::adapters::personal_archive {
namespace {

} // namespace

base::Result<AuthenticatedArchiveMetadata>
authenticate_archive_metadata(const ArchiveOpenRequest& request) {
    if (request.source.empty() || request.maximum_metadata_size == 0) {
        return base::Result<AuthenticatedArchiveMetadata>::failure(
            {base::ErrorCode::kInvalidArgument, "archive open request is invalid"});
    }
    detail::Win32InputFile input;
    if (auto opened = input.open(request.source); !opened) {
        return base::Result<AuthenticatedArchiveMetadata>::failure(
            {base::ErrorCode::kIoFailure, "failed to open personal archive"});
    }
    auto file_size = input.size();
    if (!file_size) {
        return base::Result<AuthenticatedArchiveMetadata>::failure(
            {base::ErrorCode::kIoFailure, "failed to size personal archive"});
    }
    auto preamble = detail::read_archive_preamble(input, request, file_size.value());
    if (!preamble) {
        return base::Result<AuthenticatedArchiveMetadata>::failure(preamble.error());
    }
    AuthenticatedArchiveMetadata metadata;
    metadata.content_kind = preamble.value().header.content_kind;
    metadata.manifest_content_kind = preamble.value().manifest.content_kind;
    metadata.backup_type = detail::archive_backup_type(preamble.value().header);
    metadata.file_uuid = preamble.value().header.file_uuid;
    metadata.backup_set_uuid = preamble.value().header.backup_set_uuid;
    metadata.parent_uuid = preamble.value().header.parent_uuid;
    metadata.encryption_enabled =
        (preamble.value().header.flags & format::personal_archive::kBackupFlagEncrypted) != 0;
    // Prefer header content_kind (AAD-authenticated); require manifest agreement.
    if (metadata.content_kind == format::personal_archive::kContentKindVolumeSet &&
        metadata.manifest_content_kind != format::kManifestContentKindVolumeSet) {
        return base::Result<AuthenticatedArchiveMetadata>::failure(
            {base::ErrorCode::kCorruptData, "shell.archive_corrupt"});
    }
    if (metadata.content_kind == format::personal_archive::kContentKindFileSet &&
        metadata.manifest_content_kind != format::kManifestContentKindFileSet) {
        return base::Result<AuthenticatedArchiveMetadata>::failure(
            {base::ErrorCode::kCorruptData, "shell.archive_corrupt"});
    }
    if (metadata.content_kind != format::personal_archive::kContentKindVolumeSet &&
        metadata.content_kind != format::personal_archive::kContentKindFileSet) {
        return base::Result<AuthenticatedArchiveMetadata>::failure(
            {base::ErrorCode::kUnsupportedVersion, "shell.unsupported_content_kind"});
    }
    return base::Result<AuthenticatedArchiveMetadata>::success(std::move(metadata));
}

} // namespace aegra::adapters::personal_archive
