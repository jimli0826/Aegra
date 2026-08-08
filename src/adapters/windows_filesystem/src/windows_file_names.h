#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::adapters::windows_filesystem::detail {

[[nodiscard]] contracts::EncodedName make_utf16_name(const std::wstring_view name);
[[nodiscard]] base::Result<std::wstring>
join_relative_path(const std::vector<std::uint16_t>& root_utf16,
                   const std::vector<contracts::EncodedName>& components);
[[nodiscard]] base::Result<void> validate_component(const contracts::EncodedName& name);
[[nodiscard]] std::vector<std::uint16_t> to_utf16_vector(const std::wstring& value);

} // namespace aegra::adapters::windows_filesystem::detail
