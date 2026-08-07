#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace aegra::ports {

// Byte-offset random access over a logical image (for example a whole-disk view).
// Implementations must not modify the underlying recovery-point bytes.
class IRandomAccessReader {
  public:
    IRandomAccessReader() = default;
    virtual ~IRandomAccessReader() = default;
    IRandomAccessReader(const IRandomAccessReader&) = delete;
    IRandomAccessReader& operator=(const IRandomAccessReader&) = delete;
    IRandomAccessReader(IRandomAccessReader&&) = delete;
    IRandomAccessReader& operator=(IRandomAccessReader&&) = delete;

    [[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;

    // Reads up to destination.size() bytes starting at offset.
    // Short reads at EOF are allowed; returns bytes actually copied.
    // Unmapped holes must be zero-filled by the implementation.
    [[nodiscard]] virtual base::Result<std::size_t>
    read_at(std::uint64_t offset, std::span<std::byte> destination,
            base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
