#include "aegra/adapters/compression_zstd/zstd_codec.h"

#include "aegra/base/error.h"

#include <zstd.h>

#include <string>
#include <utility>

namespace aegra::adapters::compression_zstd {
namespace {

[[nodiscard]] base::Error invalid(std::string message) {
    return {base::ErrorCode::kInvalidArgument, std::move(message)};
}

[[nodiscard]] base::Error corrupt(const std::size_t code) {
    return {base::ErrorCode::kCorruptData,
            std::string("Zstandard operation failed: ") + ZSTD_getErrorName(code)};
}

} // namespace

base::Result<std::vector<std::byte>> compress(std::span<const std::byte> input,
                                              const int compression_level) {
    if (input.empty()) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("cannot compress an empty payload"));
    }
    const auto bound = ZSTD_compressBound(input.size());
    if (ZSTD_isError(bound) != 0U) {
        return base::Result<std::vector<std::byte>>::failure(corrupt(bound));
    }
    std::vector<std::byte> output(bound);
    const auto written =
        ZSTD_compress(output.data(), output.size(), input.data(), input.size(), compression_level);
    if (ZSTD_isError(written) != 0U) {
        return base::Result<std::vector<std::byte>>::failure(corrupt(written));
    }
    output.resize(written);
    return base::Result<std::vector<std::byte>>::success(std::move(output));
}

base::Result<std::vector<std::byte>> decompress(std::span<const std::byte> input,
                                                const std::size_t expected_size,
                                                const std::size_t maximum_output_size) {
    if (input.empty() || expected_size == 0) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("compressed input and expected size must be non-zero"));
    }
    if (expected_size > maximum_output_size) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("decompressed payload exceeds the configured output limit"));
    }
    std::vector<std::byte> output(expected_size);
    const auto written = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(written) != 0U) {
        return base::Result<std::vector<std::byte>>::failure(corrupt(written));
    }
    if (written != expected_size) {
        return base::Result<std::vector<std::byte>>::failure(
            {base::ErrorCode::kCorruptData, "decompressed payload size does not match metadata"});
    }
    return base::Result<std::vector<std::byte>>::success(std::move(output));
}

} // namespace aegra::adapters::compression_zstd
