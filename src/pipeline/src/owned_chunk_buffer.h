#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace aegra::pipeline::detail {

constexpr std::size_t kChunkBufferAlignment = 64U * 1024U;

struct OwnedChunkBuffer final {
    std::vector<std::byte> storage;
    std::size_t payload_offset{0};
    std::size_t payload_size{0};

    [[nodiscard]] std::span<std::byte> bytes() noexcept {
        return std::span<std::byte>(storage).subspan(payload_offset, payload_size);
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return std::span<const std::byte>(storage).subspan(payload_offset, payload_size);
    }

    [[nodiscard]] static OwnedChunkBuffer from_contiguous(std::vector<std::byte> payload) noexcept {
        const auto size = payload.size();
        return OwnedChunkBuffer{std::move(payload), 0, size};
    }
};

[[nodiscard]] inline std::size_t aligned_payload_offset(const std::byte* data) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    const auto remainder = address % kChunkBufferAlignment;
    return remainder == 0 ? 0 : kChunkBufferAlignment - remainder;
}

} // namespace aegra::pipeline::detail
