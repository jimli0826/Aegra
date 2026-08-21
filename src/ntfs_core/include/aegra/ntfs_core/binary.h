#pragma once

#include "aegra/base/error.h"
#include "aegra/ntfs_core/types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace aegra::ntfs_core {

[[nodiscard]] base::Error make_error(base::ErrorCode code, std::string message_code);

[[nodiscard]] bool checked_add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;
[[nodiscard]] bool checked_mul_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> data, std::size_t offset);
[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> data, std::size_t offset);
[[nodiscard]] std::uint64_t read_u64(std::span<const std::byte> data, std::size_t offset);
[[nodiscard]] std::int64_t read_signed_le(std::span<const std::byte> data, std::size_t offset,
                                          std::size_t length);

void write_u16(std::span<std::byte> data, std::size_t offset, std::uint16_t value);
void write_unsigned_le(std::span<std::byte> data, std::size_t offset, std::uint64_t value,
                       std::size_t length);
void write_signed_le(std::span<std::byte> data, std::size_t offset, std::int64_t value,
                     std::size_t length);

[[nodiscard]] FileReference unpack_file_reference(std::uint64_t packed) noexcept;
[[nodiscard]] std::uint64_t pack_file_reference(FileReference reference) noexcept;

} // namespace aegra::ntfs_core
