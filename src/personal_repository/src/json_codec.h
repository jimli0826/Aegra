#pragma once

#include "aegra/base/result.h"
#include "aegra/personal_repository/catalog.h"

#include <nlohmann/json.hpp>

#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace aegra::personal_repository::detail {

using Json = nlohmann::json;

[[nodiscard]] base::Error invalid(std::string message);
[[nodiscard]] base::Error corrupt(std::string message);

[[nodiscard]] base::Result<Json> parse_json_object(std::string_view encoded,
                                                   const CatalogCodecLimits& limits);
[[nodiscard]] base::Result<void> require_exact_keys(const Json& value,
                                                    std::span<const std::string_view> expected);

[[nodiscard]] bool is_canonical_uuid(std::string_view value) noexcept;
[[nodiscard]] bool is_repository_key(std::string_view value) noexcept;
[[nodiscard]] bool is_archive_main_key(std::string_view key, std::string_view file_uuid) noexcept;
[[nodiscard]] bool is_archive_member_key(std::string_view key,
                                         std::string_view archive_main_key) noexcept;

[[nodiscard]] std::string backup_type_name(format::BackupType type);
[[nodiscard]] base::Result<format::BackupType> parse_backup_type(const Json& value);

template <typename T>
[[nodiscard]] base::Result<T> get_required(const Json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end()) {
        return base::Result<T>::failure(corrupt("repository document is missing a field"));
    }
    try {
        return base::Result<T>::success(found->get<T>());
    } catch (const nlohmann::json::exception&) {
        return base::Result<T>::failure(corrupt("repository document field has invalid type"));
    }
}

template <typename T>
[[nodiscard]] base::Result<T> get_unsigned(const Json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_number_unsigned()) {
        return base::Result<T>::failure(
            corrupt("repository document unsigned field has invalid type"));
    }
    const auto raw = found->get<std::uint64_t>();
    if (raw > static_cast<std::uint64_t>((std::numeric_limits<T>::max)())) {
        return base::Result<T>::failure(
            corrupt("repository document unsigned field is out of range"));
    }
    return base::Result<T>::success(static_cast<T>(raw));
}

} // namespace aegra::personal_repository::detail
