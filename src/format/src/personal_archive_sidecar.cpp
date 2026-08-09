#include "aegra/format/personal_archive_sidecar.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace aegra::format::personal_archive {
namespace {

constexpr std::array<char, 8> kSidecarMagic = {'M', 'Y', 'B', 'K', 'H', 'I', 'D', 'X'};
inline constexpr std::size_t kRecordSize = 1 + kSidecarHashSize;

struct DecodedVolume final {
    SidecarVolume volume;
    std::size_t next_offset{0};
};

[[nodiscard]] base::Error corrupt(std::string message) {
    return {base::ErrorCode::kCorruptData, std::move(message)};
}

template <typename Integer>
void write_integer(std::span<std::byte> bytes, const std::size_t offset, Integer value) {
    static_assert(std::is_unsigned_v<Integer>);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes[offset + index] = static_cast<std::byte>(value & 0xFFU);
        value >>= 8U;
    }
}

template <typename Integer>
[[nodiscard]] Integer read_integer(std::span<const std::byte> bytes, const std::size_t offset) {
    static_assert(std::is_unsigned_v<Integer>);
    Integer value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Integer>(std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

template <std::size_t Size>
void write_array(std::span<std::byte> output, const std::size_t offset,
                 const std::array<std::byte, Size>& value) {
    std::copy(value.begin(), value.end(), output.begin() + static_cast<std::ptrdiff_t>(offset));
}

template <std::size_t Size>
[[nodiscard]] std::array<std::byte, Size> read_array(std::span<const std::byte> input,
                                                     const std::size_t offset) {
    std::array<std::byte, Size> result{};
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), Size, result.begin());
    return result;
}

[[nodiscard]] bool valid_record(const SidecarRecord& record) {
    if (record.state == SidecarBlockState::kData) {
        return true;
    }
    if (record.state != SidecarBlockState::kZero && record.state != SidecarBlockState::kFree) {
        return false;
    }
    return std::all_of(record.hash.begin(), record.hash.end(),
                       [](const std::byte value) { return value == std::byte{0}; });
}

[[nodiscard]] base::Result<void> validate_header(const SidecarHeader& header) {
    if (header.block_size == 0 || header.hash_algorithm != SidecarHashAlgorithm::kSha256 ||
        header.hash_size != kSidecarHashSize ||
        header.compression_method != SidecarCompressionMethod::kZstandard) {
        return base::Result<void>::failure(corrupt("sidecar algorithms are invalid"));
    }
    const bool encrypted = (header.flags & kSidecarFlagEncrypted) != 0;
    if (encrypted) {
        if (header.flags != kSidecarFlagEncrypted ||
            header.encryption_method != SidecarEncryptionMethod::kXChaCha20Poly1305) {
            return base::Result<void>::failure(corrupt("sidecar encryption is invalid"));
        }
    } else if (header.flags != 0 || header.encryption_method != SidecarEncryptionMethod::kNone) {
        return base::Result<void>::failure(corrupt("unencrypted sidecar flags are invalid"));
    }
    if (header.volume_count == 0 || header.payload_uncompressed_size == 0 ||
        header.payload_stored_size == 0) {
        return base::Result<void>::failure(corrupt("sidecar sizes are invalid"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::size_t> encoded_payload_size(const SidecarPayload& payload) {
    if (payload.volumes.empty() ||
        payload.volumes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return base::Result<std::size_t>::failure(corrupt("sidecar volume count is invalid"));
    }
    std::size_t size = 0;
    std::uint32_t previous_index = 0;
    bool first = true;
    for (const auto& volume : payload.volumes) {
        if (volume.records.empty() || (!first && volume.volume_index <= previous_index) ||
            volume.records.size() >
                ((std::numeric_limits<std::size_t>::max)() - size - kSidecarVolumeHeaderSize) /
                    kRecordSize) {
            return base::Result<std::size_t>::failure(corrupt("sidecar volume is invalid"));
        }
        size += kSidecarVolumeHeaderSize + volume.records.size() * kRecordSize;
        previous_index = volume.volume_index;
        first = false;
    }
    return base::Result<std::size_t>::success(size);
}

[[nodiscard]] base::Result<DecodedVolume> decode_volume(const std::span<const std::byte> bytes,
                                                        const std::size_t start_offset) {
    if (start_offset > bytes.size() || bytes.size() - start_offset < kSidecarVolumeHeaderSize) {
        return base::Result<DecodedVolume>::failure(corrupt("sidecar payload is truncated"));
    }
    SidecarVolume volume;
    volume.volume_index = read_integer<std::uint32_t>(bytes, start_offset);
    const auto count = read_integer<std::uint64_t>(bytes, start_offset + 4);
    auto offset = start_offset + kSidecarVolumeHeaderSize;
    if (count == 0 || count > (bytes.size() - offset) / kRecordSize) {
        return base::Result<DecodedVolume>::failure(corrupt("sidecar volume is invalid"));
    }
    volume.records.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        SidecarRecord record;
        record.state = static_cast<SidecarBlockState>(std::to_integer<std::uint8_t>(bytes[offset]));
        record.hash = read_array<kSidecarHashSize>(bytes, offset + 1);
        if (!valid_record(record)) {
            return base::Result<DecodedVolume>::failure(corrupt("sidecar record is invalid"));
        }
        volume.records.push_back(record);
        offset += kRecordSize;
    }
    return base::Result<DecodedVolume>::success({std::move(volume), offset});
}

} // namespace

base::Result<EncodedSidecarHeader> encode_sidecar_header(const SidecarHeader& header) {
    auto validation = validate_header(header);
    if (!validation) {
        return base::Result<EncodedSidecarHeader>::failure(validation.error());
    }
    EncodedSidecarHeader output{};
    std::transform(kSidecarMagic.begin(), kSidecarMagic.end(), output.begin(),
                   [](const char value) { return static_cast<std::byte>(value); });
    write_integer<std::uint16_t>(output, 8, kSidecarVersion);
    write_integer(output, 10, header.flags);
    write_integer(output, 12, header.block_size);
    write_array(output, 16, header.file_uuid);
    output[32] = static_cast<std::byte>(header.hash_algorithm);
    output[33] = static_cast<std::byte>(header.hash_size);
    output[34] = static_cast<std::byte>(header.compression_method);
    output[35] = static_cast<std::byte>(header.encryption_method);
    write_integer(output, 36, header.volume_count);
    write_integer(output, 40, header.payload_uncompressed_size);
    write_integer(output, 48, header.payload_stored_size);
    write_array(output, 56, header.nonce);
    write_array(output, 80, header.authentication_tag);
    return base::Result<EncodedSidecarHeader>::success(output);
}

base::Result<SidecarHeader> decode_sidecar_header(std::span<const std::byte> bytes) {
    if (bytes.size() < kSidecarHeaderSize ||
        !std::equal(kSidecarMagic.begin(), kSidecarMagic.end(), bytes.begin(),
                    [](const char expected, const std::byte actual) {
                        return static_cast<std::byte>(expected) == actual;
                    }) ||
        read_integer<std::uint16_t>(bytes, 8) != kSidecarVersion) {
        return base::Result<SidecarHeader>::failure(corrupt("sidecar header is invalid"));
    }
    SidecarHeader result;
    result.flags = read_integer<std::uint16_t>(bytes, 10);
    result.block_size = read_integer<std::uint32_t>(bytes, 12);
    result.file_uuid = read_array<16>(bytes, 16);
    result.hash_algorithm =
        static_cast<SidecarHashAlgorithm>(std::to_integer<std::uint8_t>(bytes[32]));
    result.hash_size = std::to_integer<std::uint8_t>(bytes[33]);
    result.compression_method =
        static_cast<SidecarCompressionMethod>(std::to_integer<std::uint8_t>(bytes[34]));
    result.encryption_method =
        static_cast<SidecarEncryptionMethod>(std::to_integer<std::uint8_t>(bytes[35]));
    result.volume_count = read_integer<std::uint32_t>(bytes, 36);
    result.payload_uncompressed_size = read_integer<std::uint64_t>(bytes, 40);
    result.payload_stored_size = read_integer<std::uint64_t>(bytes, 48);
    result.nonce = read_array<24>(bytes, 56);
    result.authentication_tag = read_array<16>(bytes, 80);
    auto validation = validate_header(result);
    return validation ? base::Result<SidecarHeader>::success(result)
                      : base::Result<SidecarHeader>::failure(validation.error());
}

base::Result<std::vector<std::byte>> encode_sidecar_payload(const SidecarPayload& payload) {
    auto size = encoded_payload_size(payload);
    if (!size) {
        return base::Result<std::vector<std::byte>>::failure(size.error());
    }
    std::vector<std::byte> output(size.value());
    std::size_t offset = 0;
    for (const auto& volume : payload.volumes) {
        write_integer(output, offset, volume.volume_index);
        write_integer(output, offset + 4, static_cast<std::uint64_t>(volume.records.size()));
        offset += kSidecarVolumeHeaderSize;
        for (const auto& record : volume.records) {
            if (!valid_record(record)) {
                return base::Result<std::vector<std::byte>>::failure(
                    corrupt("sidecar record is invalid"));
            }
            output[offset] = static_cast<std::byte>(record.state);
            write_array(output, offset + 1, record.hash);
            offset += kRecordSize;
        }
    }
    return base::Result<std::vector<std::byte>>::success(std::move(output));
}

base::Result<SidecarPayload> decode_sidecar_payload(std::span<const std::byte> bytes,
                                                    const std::uint32_t expected_volume_count) {
    SidecarPayload result;
    result.volumes.reserve(expected_volume_count);
    std::size_t offset = 0;
    for (std::uint32_t index = 0; index < expected_volume_count; ++index) {
        auto decoded = decode_volume(bytes, offset);
        if (!decoded) {
            return base::Result<SidecarPayload>::failure(decoded.error());
        }
        if (!result.volumes.empty() &&
            decoded.value().volume.volume_index <= result.volumes.back().volume_index) {
            return base::Result<SidecarPayload>::failure(
                corrupt("sidecar volumes are not ordered"));
        }
        offset = decoded.value().next_offset;
        result.volumes.push_back(std::move(decoded).value().volume);
    }
    if (offset != bytes.size()) {
        return base::Result<SidecarPayload>::failure(corrupt("sidecar payload has trailing data"));
    }
    return base::Result<SidecarPayload>::success(std::move(result));
}

} // namespace aegra::format::personal_archive
