#pragma once

#include <cstddef>
#include <cstdint>

namespace aegra::adapters::dokan::detail {

class Crc32c final {
  public:
    [[nodiscard]] static std::uint32_t compute(const void* data, std::size_t len,
                                               std::uint32_t seed = 0) noexcept;

  private:
    static const std::uint32_t kTable[256];
};

} // namespace aegra::adapters::dokan::detail
