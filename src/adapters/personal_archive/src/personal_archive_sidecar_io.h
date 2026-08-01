#pragma once

#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/base/result.h"
#include "aegra/format/personal_archive_sidecar.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace aegra::adapters::personal_archive::detail {

struct SidecarWriteRequest final {
    std::filesystem::path destination;
    std::string_view password;
    std::array<std::byte, 16> file_uuid{};
    std::uint32_t block_size{0};
    std::uint32_t volume_index{0};
    std::span<const format::personal_archive::SidecarRecord> records;
    crypto_sodium::KdfParameters kdf;
    std::array<std::byte, crypto_sodium::kMetadataSaltSize> salt{};
};

[[nodiscard]] base::Result<void> write_sidecar(const SidecarWriteRequest& request);

} // namespace aegra::adapters::personal_archive::detail
