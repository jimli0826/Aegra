#pragma once

#include "aegra/base/result.h"

#include <cstddef>
#include <span>
#include <vector>

namespace aegra::adapters::compression_zstd {

inline constexpr int kDefaultCompressionLevel = 3;

/// One-shot helpers (allocate a temporary ZSTD context per call). Prefer
/// `ZstdCompressor` on hot paths that compress many blocks.
[[nodiscard]] base::Result<std::vector<std::byte>>
compress(std::span<const std::byte> input, int compression_level = kDefaultCompressionLevel);

[[nodiscard]] base::Result<std::vector<std::byte>> decompress(std::span<const std::byte> input,
                                                              std::size_t expected_size,
                                                              std::size_t maximum_output_size);

/// Thread-local / worker-local compressor: reuses one ZSTD_CCtx and an output scratch buffer.
/// Not thread-safe; own one instance per concurrent worker.
class ZstdCompressor final {
  public:
    ZstdCompressor();
    ~ZstdCompressor();
    ZstdCompressor(const ZstdCompressor&) = delete;
    ZstdCompressor& operator=(const ZstdCompressor&) = delete;
    ZstdCompressor(ZstdCompressor&& other) noexcept;
    ZstdCompressor& operator=(ZstdCompressor&& other) noexcept;

    /// Compresses `input` into the internal scratch buffer and returns a copy of the written
    /// bytes (caller decides whether the compressed form is smaller than the source).
    [[nodiscard]] base::Result<std::vector<std::byte>>
    compress(std::span<const std::byte> input, int compression_level = kDefaultCompressionLevel);

  private:
    void* context_{nullptr}; // ZSTD_CCtx*
    std::vector<std::byte> scratch_;
};

/// Thread-local / worker-local decompressor that reuses one ZSTD_DCtx.
/// Not thread-safe; own one instance per concurrent worker.
class ZstdDecompressor final {
  public:
    ZstdDecompressor();
    ~ZstdDecompressor();
    ZstdDecompressor(const ZstdDecompressor&) = delete;
    ZstdDecompressor& operator=(const ZstdDecompressor&) = delete;
    ZstdDecompressor(ZstdDecompressor&& other) noexcept;
    ZstdDecompressor& operator=(ZstdDecompressor&& other) noexcept;

    [[nodiscard]] base::Result<void> decompress_into(std::span<const std::byte> input,
                                                     std::span<std::byte> output,
                                                     std::size_t maximum_output_size);

  private:
    void* context_{nullptr}; // ZSTD_DCtx*
};

} // namespace aegra::adapters::compression_zstd
