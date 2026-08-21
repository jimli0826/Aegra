#pragma once

#include "aegra/base/error.h"
#include "aegra/base/result.h"

#include <string>
#include <utility>

namespace aegra::ntfs_resize::detail {

[[nodiscard]] inline base::Error make_shrink_error(const base::ErrorCode code,
                                                   std::string message_code) {
    return {code, std::move(message_code)};
}

template <typename T>
[[nodiscard]] base::Result<T> shrink_fail(const base::ErrorCode code, std::string message_code) {
    return base::Result<T>::failure(make_shrink_error(code, std::move(message_code)));
}

[[nodiscard]] inline base::Result<void> shrink_fail_void(const base::ErrorCode code,
                                                         std::string message_code) {
    return base::Result<void>::failure(make_shrink_error(code, std::move(message_code)));
}

} // namespace aegra::ntfs_resize::detail
