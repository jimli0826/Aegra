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

[[nodiscard]] base::Result<std::vector<std::byte>>
compress_with_context(ZSTD_CCtx* context, std::vector<std::byte>& scratch,
                      const std::span<const std::byte> input, const int compression_level) {
    if (context == nullptr) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("Zstandard compressor context is not initialized"));
    }
    if (input.empty()) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("cannot compress an empty payload"));
    }
    const auto bound = ZSTD_compressBound(input.size());
    if (ZSTD_isError(bound) != 0U) {
        return base::Result<std::vector<std::byte>>::failure(corrupt(bound));
    }
    if (scratch.size() < bound) {
        scratch.resize(bound);
    }
    const auto written =
        ZSTD_compressCCtx(context, scratch.data(), bound, input.data(), input.size(),
                          compression_level);
    if (ZSTD_isError(written) != 0U) {
        return base::Result<std::vector<std::byte>>::failure(corrupt(written));
    }
    return base::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(scratch.begin(),
                               scratch.begin() + static_cast<std::ptrdiff_t>(written)));
}

} // namespace

base::Result<std::vector<std::byte>> compress(std::span<const std::byte> input,
                                              const int compression_level) {
    ZstdCompressor compressor;
    return compressor.compress(input, compression_level);
}

base::Result<std::vector<std::byte>> decompress(std::span<const std::byte> input,
                                                const std::size_t expected_size,
                                                const std::size_t maximum_output_size) {
    std::vector<std::byte> output(expected_size);
    ZstdDecompressor decompressor;
    auto decompressed = decompressor.decompress_into(input, output, maximum_output_size);
    if (!decompressed) {
        return base::Result<std::vector<std::byte>>::failure(decompressed.error());
    }
    return base::Result<std::vector<std::byte>>::success(std::move(output));
}

ZstdCompressor::ZstdCompressor() : context_(ZSTD_createCCtx()) {}

ZstdCompressor::~ZstdCompressor() {
    if (context_ != nullptr) {
        ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(context_));
        context_ = nullptr;
    }
}

ZstdCompressor::ZstdCompressor(ZstdCompressor&& other) noexcept
    : context_(other.context_), scratch_(std::move(other.scratch_)) {
    other.context_ = nullptr;
}

ZstdCompressor& ZstdCompressor::operator=(ZstdCompressor&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (context_ != nullptr) {
        ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(context_));
    }
    context_ = other.context_;
    other.context_ = nullptr;
    scratch_ = std::move(other.scratch_);
    return *this;
}

base::Result<std::vector<std::byte>> ZstdCompressor::compress(const std::span<const std::byte> input,
                                                             const int compression_level) {
    return compress_with_context(static_cast<ZSTD_CCtx*>(context_), scratch_, input,
                                 compression_level);
}

ZstdDecompressor::ZstdDecompressor() : context_(ZSTD_createDCtx()) {}

ZstdDecompressor::~ZstdDecompressor() {
    if (context_ != nullptr) {
        ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(context_));
        context_ = nullptr;
    }
}

ZstdDecompressor::ZstdDecompressor(ZstdDecompressor&& other) noexcept
    : context_(other.context_) {
    other.context_ = nullptr;
}

ZstdDecompressor& ZstdDecompressor::operator=(ZstdDecompressor&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (context_ != nullptr) {
        ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(context_));
    }
    context_ = other.context_;
    other.context_ = nullptr;
    return *this;
}

base::Result<void>
ZstdDecompressor::decompress_into(const std::span<const std::byte> input,
                                  const std::span<std::byte> output,
                                  const std::size_t maximum_output_size) {
    if (context_ == nullptr) {
        return base::Result<void>::failure(
            invalid("Zstandard decompressor context is not initialized"));
    }
    if (input.empty() || output.empty()) {
        return base::Result<void>::failure(
            invalid("compressed input and expected size must be non-zero"));
    }
    if (output.size() > maximum_output_size) {
        return base::Result<void>::failure(
            invalid("decompressed payload exceeds the configured output limit"));
    }
    const auto written = ZSTD_decompressDCtx(static_cast<ZSTD_DCtx*>(context_), output.data(),
                                             output.size(), input.data(), input.size());
    if (ZSTD_isError(written) != 0U) {
        return base::Result<void>::failure(corrupt(written));
    }
    if (written != output.size()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "decompressed payload size does not match metadata"});
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::compression_zstd
