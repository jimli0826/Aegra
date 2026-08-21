#include "shrink_plan_internal.h"

#include <cstring>

namespace aegra::ntfs_resize::detail {
namespace {

constexpr std::uint32_t rotr(const std::uint32_t value, const std::uint32_t bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
}

void store_be32(std::byte* out, const std::uint32_t value) noexcept {
    out[0] = static_cast<std::byte>((value >> 24) & 0xFFU);
    out[1] = static_cast<std::byte>((value >> 16) & 0xFFU);
    out[2] = static_cast<std::byte>((value >> 8) & 0xFFU);
    out[3] = static_cast<std::byte>(value & 0xFFU);
}

void compress_block(std::uint32_t state[8], const std::byte block[64]) noexcept {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
        0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
        0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
        0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    std::uint32_t w[64];
    for (std::uint32_t i = 0; i < 16; ++i) {
        const std::size_t o = static_cast<std::size_t>(i) * 4U;
        w[i] = (std::to_integer<std::uint32_t>(block[o]) << 24) |
               (std::to_integer<std::uint32_t>(block[o + 1]) << 16) |
               (std::to_integer<std::uint32_t>(block[o + 2]) << 8) |
               std::to_integer<std::uint32_t>(block[o + 3]);
    }
    for (std::uint32_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];
    for (std::uint32_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace

std::array<std::byte, 32> sha256(const std::span<const std::byte> data) {
    std::uint32_t state[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                              0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::byte block[64];
    std::size_t offset = 0;
    while (offset + 64 <= data.size()) {
        std::memcpy(block, data.data() + offset, 64);
        compress_block(state, block);
        offset += 64;
    }

    const std::size_t rem = data.size() - offset;
    std::memset(block, 0, sizeof(block));
    if (rem != 0) {
        std::memcpy(block, data.data() + offset, rem);
    }
    block[rem] = static_cast<std::byte>(0x80);
    if (rem >= 56) {
        compress_block(state, block);
        std::memset(block, 0, sizeof(block));
    }
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8U;
    store_be32(block + 56, static_cast<std::uint32_t>(bit_len >> 32));
    store_be32(block + 60, static_cast<std::uint32_t>(bit_len & 0xFFFFFFFFU));
    compress_block(state, block);

    std::array<std::byte, 32> out{};
    for (std::uint32_t i = 0; i < 8; ++i) {
        store_be32(out.data() + static_cast<std::size_t>(i) * 4U, state[i]);
    }
    return out;
}

} // namespace aegra::ntfs_resize::detail
