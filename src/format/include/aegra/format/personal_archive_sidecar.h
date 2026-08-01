#pragma once

#include "aegra/base/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aegra::format::personal_archive {

inline constexpr std::uint16_t kSidecarVersion = 1;
inline constexpr std::size_t kSidecarHeaderSize = 96;
inline constexpr std::size_t kSidecarVolumeHeaderSize = 12;
inline constexpr std::size_t kSidecarHashSize = 32;
inline constexpr std::uint16_t kSidecarFlagEncrypted = 0x0001;

enum class SidecarHashAlgorithm : std::uint8_t {
    kSha256 = 2,
};

enum class SidecarCompressionMethod : std::uint8_t {
    kZstandard = 1,
};

enum class SidecarEncryptionMethod : std::uint8_t {
    kXChaCha20Poly1305 = 2,
};

enum class SidecarBlockState : std::uint8_t {
    kData = 0,
    kZero = 1,
    kSkip = 2,
};

struct SidecarHeader final {
    std::uint16_t flags{kSidecarFlagEncrypted};
    std::uint32_t block_size{0};
    std::array<std::byte, 16> file_uuid{};
    SidecarHashAlgorithm hash_algorithm{SidecarHashAlgorithm::kSha256};
    std::uint8_t hash_size{kSidecarHashSize};
    SidecarCompressionMethod compression_method{SidecarCompressionMethod::kZstandard};
    SidecarEncryptionMethod encryption_method{SidecarEncryptionMethod::kXChaCha20Poly1305};
    std::uint32_t volume_count{0};
    std::uint64_t payload_uncompressed_size{0};
    std::uint64_t payload_stored_size{0};
    std::array<std::byte, 24> nonce{};
    std::array<std::byte, 16> authentication_tag{};
};

struct SidecarRecord final {
    SidecarBlockState state{SidecarBlockState::kData};
    std::array<std::byte, kSidecarHashSize> hash{};
};

struct SidecarVolume final {
    std::uint32_t volume_index{0};
    std::vector<SidecarRecord> records;
};

struct SidecarPayload final {
    std::vector<SidecarVolume> volumes;
};

using EncodedSidecarHeader = std::array<std::byte, kSidecarHeaderSize>;

[[nodiscard]] base::Result<EncodedSidecarHeader> encode_sidecar_header(const SidecarHeader& header);
[[nodiscard]] base::Result<SidecarHeader> decode_sidecar_header(std::span<const std::byte> bytes);
[[nodiscard]] base::Result<std::vector<std::byte>>
encode_sidecar_payload(const SidecarPayload& payload);
[[nodiscard]] base::Result<SidecarPayload>
decode_sidecar_payload(std::span<const std::byte> bytes, std::uint32_t expected_volume_count);

} // namespace aegra::format::personal_archive
