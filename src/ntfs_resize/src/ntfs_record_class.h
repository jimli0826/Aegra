#pragma once

#include <cstdint>

namespace aegra::ntfs_resize::detail {

enum class RecordClassFilter : std::uint8_t {
    kOrdinary = 1,
    kCritical = 2,
    kAll = 3,
};

[[nodiscard]] inline bool is_critical_system_record(const std::uint64_t record_number) noexcept {
    return record_number <= 11U;
}

[[nodiscard]] inline bool matches_record_filter(const RecordClassFilter filter,
                                                const std::uint64_t record_number) noexcept {
    switch (filter) {
    case RecordClassFilter::kOrdinary:
        return !is_critical_system_record(record_number);
    case RecordClassFilter::kCritical:
        return is_critical_system_record(record_number);
    case RecordClassFilter::kAll:
        return true;
    }
    return false;
}

} // namespace aegra::ntfs_resize::detail
