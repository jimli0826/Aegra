#pragma once

#include "aegra/base/error.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/shrink_plan.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::ntfs_resize::detail {

inline constexpr char kMagic0 = 'A';
inline constexpr char kMagic1 = 'G';
inline constexpr char kMagic2 = 'S';
inline constexpr char kMagic3 = 'P';
inline constexpr std::uint32_t kHeaderSize = 64;
inline constexpr std::uint32_t kSectionScalars = 1;
inline constexpr std::uint32_t kSectionProtectedRanges = 2;
inline constexpr std::uint32_t kSectionRelocations = 3;
inline constexpr std::uint32_t kSectionMutations = 4;
inline constexpr std::uint32_t kSectionCriticalOps = 5;
inline constexpr std::uint32_t kSectionCount = 5;
inline constexpr std::size_t kMaxStringBytes = 64U * 1024U;
inline constexpr std::size_t kMaxUtf16Units = 32U * 1024U;
inline constexpr std::size_t kMaxRecordCount = 1U * 1024U * 1024U;
inline constexpr std::size_t kDigestHexLen = 64;

[[nodiscard]] base::Error make_plan_error(base::ErrorCode code, std::string message_code);

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::array<std::byte, 32> sha256(std::span<const std::byte> data);
[[nodiscard]] std::string digest_to_hex(std::span<const std::byte, 32> digest);
[[nodiscard]] bool is_lowercase_hex64(std::string_view text) noexcept;

void append_u16(std::vector<std::byte>& out, std::uint16_t value);
void append_u32(std::vector<std::byte>& out, std::uint32_t value);
void append_u64(std::vector<std::byte>& out, std::uint64_t value);
void append_bytes(std::vector<std::byte>& out, std::span<const std::byte> bytes);
void append_utf8(std::vector<std::byte>& out, std::string_view text);
void append_utf16(std::vector<std::byte>& out, std::u16string_view text);

[[nodiscard]] base::Result<void> read_exact(std::span<const std::byte> data, std::size_t& offset,
                                            std::size_t size, std::span<std::byte> destination);
[[nodiscard]] base::Result<std::uint16_t> read_u16(std::span<const std::byte> data,
                                                   std::size_t& offset);
[[nodiscard]] base::Result<std::uint32_t> read_u32(std::span<const std::byte> data,
                                                   std::size_t& offset);
[[nodiscard]] base::Result<std::uint64_t> read_u64(std::span<const std::byte> data,
                                                   std::size_t& offset);
[[nodiscard]] base::Result<std::string> read_utf8(std::span<const std::byte> data,
                                                  std::size_t& offset);
[[nodiscard]] base::Result<std::u16string> read_utf16(std::span<const std::byte> data,
                                                      std::size_t& offset);

[[nodiscard]] base::Result<void> validate_plan_invariants(const ShrinkPlan& plan);
[[nodiscard]] base::Result<std::vector<std::byte>> encode_canonical_payload(const ShrinkPlan& plan);
[[nodiscard]] base::Result<void> parse_canonical_payload(std::span<const std::byte> payload,
                                                         ShrinkPlanBuilder& builder);

} // namespace aegra::ntfs_resize::detail
