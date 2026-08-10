#pragma once

#include "aegra/base/result.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace aegra::adapters::personal_archive::detail {

using CngSha256Digest = std::array<std::byte, 32>;

/// Per-worker reusable Windows CNG SHA-256 state. The provider and hash object stay open across
/// blocks; BCryptFinishHash resets a reusable hash for the next block.
class WindowsCngSha256 final {
  public:
    WindowsCngSha256();
    ~WindowsCngSha256();
    WindowsCngSha256(const WindowsCngSha256&) = delete;
    WindowsCngSha256& operator=(const WindowsCngSha256&) = delete;
    WindowsCngSha256(WindowsCngSha256&&) noexcept;
    WindowsCngSha256& operator=(WindowsCngSha256&&) noexcept;

    [[nodiscard]] base::Result<CngSha256Digest> hash(std::span<const std::byte> input);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::adapters::personal_archive::detail
