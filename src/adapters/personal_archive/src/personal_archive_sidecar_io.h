#pragma once

#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/base/result.h"
#include "aegra/format/personal_archive_sidecar.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace aegra::adapters::personal_archive::detail {

struct SidecarWriteRequest final {
    std::filesystem::path destination;
    std::string_view password;
    std::array<std::byte, 16> file_uuid{};
    std::uint32_t block_size{0};
    const format::personal_archive::SidecarPayload& payload;
    crypto_sodium::KdfParameters kdf;
    std::array<std::byte, crypto_sodium::kMetadataSaltSize> salt{};
};

[[nodiscard]] base::Result<void> write_sidecar(const SidecarWriteRequest& request);

} // namespace aegra::adapters::personal_archive::detail
