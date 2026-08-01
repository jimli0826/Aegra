#pragma once

#include <cstdint>

namespace aegra::ports {

class IClock {
public:
    IClock() = default;
    virtual ~IClock() = default;
    IClock(const IClock&) = delete;
    IClock& operator=(const IClock&) = delete;
    IClock(IClock&&) = delete;
    IClock& operator=(IClock&&) = delete;

    [[nodiscard]] virtual std::int64_t now_utc_ms() const noexcept = 0;
};

} // namespace aegra::ports
