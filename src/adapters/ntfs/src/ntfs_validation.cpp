#include "ntfs_internal.h"

#include <bit>
#include <cstring>
#include <limits>

namespace aegra::adapters::ntfs::detail {

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
    // length is 1..8 (caller validates). Never shift by >= width of the type (UB).
    if (length == 0 || length > 8) {
        return 0;
    }
    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < length; ++i) {
        raw |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[offset + i]))
               << (8U * i);
    }
    if (length == 8) {
        // Preserve the two's-complement bit pattern without an implementation-defined
        // unsigned-to-signed numeric conversion.
        return std::bit_cast<std::int64_t>(raw);
    }
    const unsigned bits = static_cast<unsigned>(length * 8U);
    const std::uint64_t sign_bit = 1ULL << (bits - 1U);
    if ((raw & sign_bit) != 0) {
        // Set all bits above the field width. bits is in [8, 56], so << bits is defined.
        raw |= (~0ULL) << bits;
    }
    return std::bit_cast<std::int64_t>(raw);
}

base::Result<void> validate_attribute_header(const std::span<const std::byte> record,
                                             const std::size_t offset, std::uint32_t& type,
                                             std::uint32_t& length, bool& non_resident) {
    if (offset > record.size() || record.size() - offset < 8) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
    }
    type = read_u32(record, offset);
    if (type == kAttrEnd) {
        length = 0;
        non_resident = false;
        return base::Result<void>::success();
    }
    if (record.size() - offset < 16) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
    }
    length = read_u32(record, offset + 4);
    if (length < 16 || length > record.size() - offset) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
    }
    non_resident = std::to_integer<std::uint8_t>(record[offset + 8]) != 0;
    return base::Result<void>::success();
}

NtfsFileReference unpack_file_reference(const std::uint64_t packed) noexcept {
    return NtfsFileReference{
        .record_number = packed & 0x0000FFFFFFFFFFFFULL,
        .sequence_number = static_cast<std::uint16_t>(packed >> 48),
    };
}

std::uint64_t pack_file_reference(const NtfsFileReference reference) noexcept {
    return (reference.record_number & 0x0000FFFFFFFFFFFFULL) |
           (static_cast<std::uint64_t>(reference.sequence_number) << 48);
}

base::Result<std::string> encode_continuation(const std::uint32_t skip_count) {
    return base::Result<std::string>::success("s:" + std::to_string(skip_count));
}

base::Result<std::uint32_t> decode_continuation(const std::optional<std::string>& token) {
    if (!token.has_value() || token->empty()) {
        return base::Result<std::uint32_t>::success(0);
    }
    if (token->size() < 3 || !token->starts_with("s:")) {
        return base::Result<std::uint32_t>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "ntfs.index_corrupt"));
    }
    try {
        const auto value = std::stoul(token->substr(2));
        if (value > (std::numeric_limits<std::uint32_t>::max)()) {
            return base::Result<std::uint32_t>::failure(
                make_error(base::ErrorCode::kInvalidArgument, "ntfs.index_corrupt"));
        }
        return base::Result<std::uint32_t>::success(static_cast<std::uint32_t>(value));
    } catch (...) {
        return base::Result<std::uint32_t>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "ntfs.index_corrupt"));
    }
}

} // namespace aegra::adapters::ntfs::detail
