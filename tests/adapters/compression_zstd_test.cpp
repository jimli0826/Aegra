#include "aegra/adapters/compression_zstd/zstd_codec.h"

#include "aegra/base/error.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

std::vector<std::byte> make_payload() {
    std::vector<std::byte> result(64ULL * 1024ULL);
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::byte>(index % 17);
    }
    return result;
}

int run_tests() {
    const auto payload = make_payload();
    const auto compressed = aegra::adapters::compression_zstd::compress(payload);
    bool passed = expect(compressed.has_value(), "Zstandard compression succeeds");
    if (!compressed) {
        return EXIT_FAILURE;
    }
    passed &= expect(compressed.value().size() < payload.size(), "repeated payload compresses");
    const auto restored = aegra::adapters::compression_zstd::decompress(
        compressed.value(), payload.size(), payload.size());
    passed &= expect(restored.has_value() && restored.value() == payload,
                     "Zstandard payload survives roundtrip");

    const auto limited = aegra::adapters::compression_zstd::decompress(
        compressed.value(), payload.size(), payload.size() - 1);
    passed &= expect(!limited.has_value(), "decompression output limit is enforced");
    passed &= expect(limited.error().code == aegra::base::ErrorCode::kInvalidArgument,
                     "output limit failure has stable error code");
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main() noexcept {
    try {
        return run_tests();
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
