#pragma once

#include "aegra/ports/block_io.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

namespace aegra::adapters::memory {

struct MemoryBlockSourceOptions final {
    std::size_t max_read_size{(std::numeric_limits<std::size_t>::max)()};
    std::optional<std::uint64_t> fail_at_offset;
};

class MemoryBlockSource final : public ports::IBlockSource {
  public:
    explicit MemoryBlockSource(std::vector<std::byte> data, MemoryBlockSourceOptions options = {});

    [[nodiscard]] std::uint64_t size_bytes() const noexcept override;
    [[nodiscard]] base::Result<std::size_t> read(std::uint64_t offset,
                                                 std::span<std::byte> destination,
                                                 base::CancellationToken cancellation) override;

  private:
    std::vector<std::byte> data_;
    MemoryBlockSourceOptions options_;
};

struct MemoryBlockSinkOptions final {
    std::optional<std::uint64_t> fail_at_offset;
    bool fail_flush{false};
};

class MemoryBlockSink final : public ports::IBlockSink {
  public:
    explicit MemoryBlockSink(std::size_t capacity, MemoryBlockSinkOptions options = {});

    [[nodiscard]] std::uint64_t capacity_bytes() const noexcept override;
    [[nodiscard]] base::Result<void> write(std::uint64_t offset, std::span<const std::byte> source,
                                           base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> flush(base::CancellationToken cancellation) override;

    [[nodiscard]] std::vector<std::byte> snapshot() const;
    [[nodiscard]] std::size_t flush_count() const;

  private:
    mutable std::mutex mutex_;
    std::vector<std::byte> data_;
    MemoryBlockSinkOptions options_;
    std::size_t flush_count_{0};
};

} // namespace aegra::adapters::memory
