#include "aegra/ntfs_resize/shrink_plan_codec.h"

#include "shrink_plan_internal.h"

#include <cstring>

namespace aegra::ntfs_resize {
namespace {

[[nodiscard]] base::Result<void> write_header(std::vector<std::byte>& out,
                                              const std::uint32_t payload_crc,
                                              const std::uint64_t payload_size,
                                              const std::array<std::byte, 32>& digest) {
    out.clear();
    out.reserve(detail::kHeaderSize + static_cast<std::size_t>(payload_size));
    out.push_back(static_cast<std::byte>(detail::kMagic0));
    out.push_back(static_cast<std::byte>(detail::kMagic1));
    out.push_back(static_cast<std::byte>(detail::kMagic2));
    out.push_back(static_cast<std::byte>(detail::kMagic3));
    detail::append_u32(out, kShrinkPlanVersion);
    detail::append_u32(out, detail::kHeaderSize);
    detail::append_u32(out, 0); // flags
    detail::append_u32(out, payload_crc);
    detail::append_u32(out, 0); // reserved
    detail::append_u64(out, payload_size);
    detail::append_bytes(out, digest);
    if (out.size() != detail::kHeaderSize) {
        return base::Result<void>::failure(detail::make_plan_error(
            base::ErrorCode::kInternal, "ntfs_resize.plan_header_size"));
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<std::vector<std::byte>> encode_shrink_plan(const ShrinkPlan& plan) {
    auto status = detail::validate_plan_invariants(plan);
    if (!status) {
        return base::Result<std::vector<std::byte>>::failure(status.error());
    }
    if (!detail::is_lowercase_hex64(plan.plan_payload_digest())) {
        return base::Result<std::vector<std::byte>>::failure(detail::make_plan_error(
            base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_digest_invalid"));
    }

    auto payload = detail::encode_canonical_payload(plan);
    if (!payload) {
        return base::Result<std::vector<std::byte>>::failure(payload.error());
    }

    const auto digest = detail::sha256(payload.value());
    const std::string digest_hex = detail::digest_to_hex(digest);
    if (digest_hex != plan.plan_payload_digest()) {
        return base::Result<std::vector<std::byte>>::failure(detail::make_plan_error(
            base::ErrorCode::kCorruptData, "ntfs_resize.plan_digest_mismatch"));
    }

    const std::uint32_t crc = detail::crc32(payload.value());
    std::vector<std::byte> encoded;
    auto header_status =
        write_header(encoded, crc, static_cast<std::uint64_t>(payload.value().size()), digest);
    if (!header_status) {
        return base::Result<std::vector<std::byte>>::failure(header_status.error());
    }
    detail::append_bytes(encoded, payload.value());
    return base::Result<std::vector<std::byte>>::success(std::move(encoded));
}

base::Result<ShrinkPlan> decode_shrink_plan(const std::span<const std::byte> bytes) {
    if (bytes.size() < detail::kHeaderSize) {
        return base::Result<ShrinkPlan>::failure(detail::make_plan_error(
            base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    if (std::to_integer<char>(bytes[0]) != detail::kMagic0 ||
        std::to_integer<char>(bytes[1]) != detail::kMagic1 ||
        std::to_integer<char>(bytes[2]) != detail::kMagic2 ||
        std::to_integer<char>(bytes[3]) != detail::kMagic3) {
        return base::Result<ShrinkPlan>::failure(detail::make_plan_error(
            base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }

    std::size_t offset = 4;
    const auto version = detail::read_u32(bytes, offset);
    const auto header_size = detail::read_u32(bytes, offset);
    const auto flags = detail::read_u32(bytes, offset);
    const auto payload_crc = detail::read_u32(bytes, offset);
    const auto reserved = detail::read_u32(bytes, offset);
    const auto payload_size = detail::read_u64(bytes, offset);
    if (!version || !header_size || !flags || !payload_crc || !reserved || !payload_size) {
        return base::Result<ShrinkPlan>::failure(detail::make_plan_error(
            base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    if (version.value() != kShrinkPlanVersion) {
        return base::Result<ShrinkPlan>::failure(detail::make_plan_error(
            base::ErrorCode::kUnsupportedVersion, "ntfs_resize.plan_unknown_version"));
    }
    if (header_size.value() != detail::kHeaderSize || flags.value() != 0 ||
        reserved.value() != 0) {
        return base::Result<ShrinkPlan>::failure(detail::make_plan_error(
            base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }

    std::array<std::byte, 32> stored_digest{};
    auto digest_status = detail::read_exact(bytes, offset, 32, stored_digest);
    if (!digest_status) {
        return base::Result<ShrinkPlan>::failure(digest_status.error());
    }
    if (offset != detail::kHeaderSize) {
        return base::Result<ShrinkPlan>::failure(detail::make_plan_error(
            base::ErrorCode::kInternal, "ntfs_resize.plan_header_size"));
    }
    if (payload_size.value() > bytes.size() - detail::kHeaderSize) {
        return base::Result<ShrinkPlan>::failure(detail::make_plan_error(
            base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    if (bytes.size() != detail::kHeaderSize + static_cast<std::size_t>(payload_size.value())) {
        return base::Result<ShrinkPlan>::failure(detail::make_plan_error(
            base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }

    const auto payload = bytes.subspan(detail::kHeaderSize,
                                       static_cast<std::size_t>(payload_size.value()));
    if (detail::crc32(payload) != payload_crc.value()) {
        return base::Result<ShrinkPlan>::failure(detail::make_plan_error(
            base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    const auto computed = detail::sha256(payload);
    if (computed != stored_digest) {
        return base::Result<ShrinkPlan>::failure(detail::make_plan_error(
            base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }

    ShrinkPlanBuilder builder;
    auto parse_status = detail::parse_canonical_payload(payload, builder);
    if (!parse_status) {
        return base::Result<ShrinkPlan>::failure(parse_status.error());
    }
    auto plan = builder.build();
    if (!plan) {
        return plan;
    }
    if (plan.value().plan_payload_digest() != detail::digest_to_hex(stored_digest)) {
        return base::Result<ShrinkPlan>::failure(detail::make_plan_error(
            base::ErrorCode::kCorruptData, "ntfs_resize.plan_digest_mismatch"));
    }
    return plan;
}

} // namespace aegra::ntfs_resize
