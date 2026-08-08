#include "windows_file_names.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <cstring>

namespace aegra::adapters::windows_filesystem::detail {
namespace {

[[nodiscard]] bool is_dot_or_dotdot(const std::wstring_view name) noexcept {
    return name == L"." || name == L"..";
}

} // namespace

contracts::EncodedName make_utf16_name(const std::wstring_view name) {
    contracts::EncodedName encoded;
    encoded.encoding = contracts::NameEncoding::kWindowsUtf16Le;
    encoded.bytes.resize(name.size() * sizeof(wchar_t));
    if (!name.empty()) {
        std::memcpy(encoded.bytes.data(), name.data(), encoded.bytes.size());
    }
    return encoded;
}

base::Result<void> validate_component(const contracts::EncodedName& name) {
    auto validated = contracts::validate_encoded_name(name);
    if (!validated) {
        return validated;
    }
    if (name.encoding != contracts::NameEncoding::kWindowsUtf16Le ||
        (name.bytes.size() % 2U) != 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "file name encoding is invalid"});
    }
    std::wstring wide(name.bytes.size() / 2U, L'\0');
    if (!name.bytes.empty()) {
        std::memcpy(wide.data(), name.bytes.data(), name.bytes.size());
    }
    if (wide.empty() || is_dot_or_dotdot(wide) ||
        wide.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "file path component is invalid"});
    }
    return base::Result<void>::success();
}

base::Result<std::wstring>
join_relative_path(const std::vector<std::uint16_t>& root_utf16,
                   const std::vector<contracts::EncodedName>& components) {
    if (root_utf16.empty()) {
        return base::Result<std::wstring>::failure(
            {base::ErrorCode::kInvalidArgument, "filesystem root is empty"});
    }
    std::wstring path(root_utf16.begin(), root_utf16.end());
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    for (const auto& component : components) {
        auto ok = validate_component(component);
        if (!ok) {
            return base::Result<std::wstring>::failure(ok.error());
        }
        std::wstring piece(component.bytes.size() / 2U, L'\0');
        if (!component.bytes.empty()) {
            std::memcpy(piece.data(), component.bytes.data(), component.bytes.size());
        }
        path.push_back(L'\\');
        path.append(piece);
    }
    return base::Result<std::wstring>::success(std::move(path));
}

std::vector<std::uint16_t> to_utf16_vector(const std::wstring& value) {
    return {value.begin(), value.end()};
}

} // namespace aegra::adapters::windows_filesystem::detail
