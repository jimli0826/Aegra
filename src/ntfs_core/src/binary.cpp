#include "aegra/ntfs_core/binary.h"

#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace aegra::ntfs_core {

base::Error make_error(const base::ErrorCode code, std::string message_code) {
    return {code, std::move(message_code)};
}

bool checked_add_u64(const std::uint64_t a, const std::uint64_t b, std::uint64_t& out) noexcept {
    if (a > (std::numeric_limits<std::uint64_t>::max)() - b) {
        return false;
    }
    out = a + b;
    return true;
}

bool checked_mul_u64(const std::uint64_t a, const std::uint64_t b, std::uint64_t& out) noexcept {
    if (a != 0 && b > (std::numeric_limits<std::uint64_t>::max)() / a) {
        return false;
    }
    out = a * b;
    return true;
}

std::uint16_t read_u16(const std::span<const std::byte> data, const std::size_t offset) {
    std::uint16_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

std::uint32_t read_u32(const std::span<const std::byte> data, const std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

std::uint64_t read_u64(const std::span<const std::byte> data, const std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

std::int64_t read_signed_le(const std::span<const std::byte> data, const std::size_t offset,
                            const std::size_t length) {
    if (length == 0 || length > 8) {
        return 0;
    }
    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < length; ++i) {
        raw |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[offset + i]))
               << (8U * i);
    }
    if (length == 8) {
        return std::bit_cast<std::int64_t>(raw);
    }
    const unsigned bits = static_cast<unsigned>(length * 8U);
    const std::uint64_t sign_bit = 1ULL << (bits - 1U);
    if ((raw & sign_bit) != 0) {
        raw |= (~0ULL) << bits;
    }
    return std::bit_cast<std::int64_t>(raw);
}

void write_u16(const std::span<std::byte> data, const std::size_t offset,
               const std::uint16_t value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

void write_unsigned_le(const std::span<std::byte> data, const std::size_t offset,
                       const std::uint64_t value, const std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
        data[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

void write_signed_le(const std::span<std::byte> data, const std::size_t offset,
                     const std::int64_t value, const std::size_t length) {
    write_unsigned_le(data, offset, std::bit_cast<std::uint64_t>(value), length);
}

FileReference unpack_file_reference(const std::uint64_t packed) noexcept {
    return FileReference{
        .record_number = packed & 0x0000FFFFFFFFFFFFULL,
        .sequence_number = static_cast<std::uint16_t>(packed >> 48),
    };
}

std::uint64_t pack_file_reference(const FileReference reference) noexcept {
    return (reference.record_number & 0x0000FFFFFFFFFFFFULL) |
           (static_cast<std::uint64_t>(reference.sequence_number) << 48);
}

} // namespace aegra::ntfs_core
