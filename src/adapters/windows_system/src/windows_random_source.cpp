#include "aegra/adapters/windows_system/windows_system.h"

#include <Windows.h>
#include <bcrypt.h>

#include <cstddef>
#include <limits>

namespace aegra::adapters::windows_system {

base::Result<void> WindowsCryptographicRandom::fill(const std::span<std::byte> destination,
                                                    const base::CancellationToken& cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kCancelled, "random generation cancelled"});
    }
    if (destination.empty()) {
        return base::Result<void>::success();
    }
    if (destination.size() > (std::numeric_limits<ULONG>::max)()) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "random request is too large"});
    }
    // BCrypt models bytes as unsigned char; the Port deliberately exposes std::byte.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* buffer = reinterpret_cast<PUCHAR>(destination.data());
    const auto status = BCryptGenRandom(nullptr, buffer, static_cast<ULONG>(destination.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInternal, "system random generation failed"});
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::windows_system
