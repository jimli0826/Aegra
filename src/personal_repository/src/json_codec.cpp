#include "json_codec.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aegra::personal_repository::detail {
namespace {

class DuplicateKeyDetector final : public nlohmann::json_sax<Json> {
  public:
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t) override { return true; }
    bool number_unsigned(number_unsigned_t) override { return true; }
    bool number_float(number_float_t, const string_t&) override { return true; }
    bool string(string_t&) override { return true; }
    bool binary(binary_t&) override { return true; }

    bool start_object(std::size_t) override {
        object_keys_.emplace_back();
        return true;
    }

    bool key(string_t& value) override {
        if (object_keys_.empty() || !object_keys_.back().insert(value).second) {
            duplicate_found_ = true;
            return false;
        }
        return true;
    }

    bool end_object() override {
        object_keys_.pop_back();
        return true;
    }

    bool start_array(std::size_t) override { return true; }
    bool end_array() override { return true; }

    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

    [[nodiscard]] bool duplicate_found() const noexcept { return duplicate_found_; }

  private:
    std::vector<std::set<std::string, std::less<>>> object_keys_;
    bool duplicate_found_{false};
};

[[nodiscard]] bool is_hex_lower(const char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

[[nodiscard]] bool has_valid_uuid_punctuation(const std::string_view value) noexcept {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
        if ((hyphen && value[index] != '-') || (!hyphen && !is_hex_lower(value[index]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_key_segment(const std::string_view segment) noexcept {
    if (segment.empty() || segment == "." || segment == "..") {
        return false;
    }
    return std::ranges::none_of(segment, [](const char value) {
        const auto byte = static_cast<unsigned char>(value);
        return value == '\\' || value == ':' || byte < 0x20 || byte == 0x7F;
    });
}

[[nodiscard]] bool all_digits(const std::string_view value) noexcept {
    return !value.empty() && std::ranges::all_of(value, [](const char digit) {
        return digit >= '0' && digit <= '9';
    });
}

[[nodiscard]] bool valid_archive_date(const std::string_view year,
                                      const std::string_view month) noexcept {
    if (year.size() != 4 || month.size() != 2 || !all_digits(year) || !all_digits(month)) {
        return false;
    }
    const auto month_value = static_cast<unsigned int>((month[0] - '0') * 10 + month[1] - '0');
    return month_value >= 1 && month_value <= 12;
}

} // namespace

base::Error invalid(std::string message) {
    return {base::ErrorCode::kInvalidArgument, std::move(message)};
}

base::Error corrupt(std::string message) {
    return {base::ErrorCode::kCorruptData, std::move(message)};
}

base::Result<Json> parse_json_object(const std::string_view encoded,
                                     const CatalogCodecLimits& limits) {
    if (encoded.empty() || encoded.size() > limits.maximum_document_bytes) {
        return base::Result<Json>::failure(corrupt("repository document size is invalid"));
    }
    DuplicateKeyDetector detector;
    if (!Json::sax_parse(encoded, &detector) || detector.duplicate_found()) {
        return base::Result<Json>::failure(corrupt("repository document JSON is invalid"));
    }
    try {
        Json parsed = Json::parse(encoded);
        if (!parsed.is_object()) {
            return base::Result<Json>::failure(
                corrupt("repository document root is not an object"));
        }
        return base::Result<Json>::success(std::move(parsed));
    } catch (const Json::exception&) {
        return base::Result<Json>::failure(corrupt("repository document JSON is invalid"));
    }
}

base::Result<void> require_exact_keys(const Json& value,
                                      const std::span<const std::string_view> expected) {
    if (value.size() != expected.size()) {
        return base::Result<void>::failure(corrupt("repository document fields are invalid"));
    }
    for (const auto key : expected) {
        if (!value.contains(key)) {
            return base::Result<void>::failure(corrupt("repository document fields are invalid"));
        }
    }
    return base::Result<void>::success();
}

bool is_canonical_uuid(const std::string_view value) noexcept {
    if (!has_valid_uuid_punctuation(value)) {
        return false;
    }
    const bool valid_version = value[14] >= '1' && value[14] <= '5';
    const bool valid_variant =
        value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b';
    return valid_version && valid_variant;
}

bool is_repository_key(const std::string_view value) noexcept {
    if (value.empty() || value.front() == '/' || value.back() == '/') {
        return false;
    }
    std::size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find('/', begin);
        const auto segment =
            value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        if (!valid_key_segment(segment)) {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

bool is_archive_main_key(const std::string_view key, const std::string_view file_uuid) noexcept {
    if (!is_repository_key(key) || !key.starts_with("archives/")) {
        return false;
    }
    const auto year_end = key.find('/', 9);
    const auto month_end =
        year_end == std::string_view::npos ? std::string_view::npos : key.find('/', year_end + 1);
    if (year_end == std::string_view::npos || month_end == std::string_view::npos ||
        key.find('/', month_end + 1) != std::string_view::npos) {
        return false;
    }
    const auto year = key.substr(9, year_end - 9);
    const auto month = key.substr(year_end + 1, month_end - year_end - 1);
    const std::string expected_name = std::string(file_uuid) + ".bkf";
    return valid_archive_date(year, month) && key.substr(month_end + 1) == expected_name;
}

bool is_archive_member_key(const std::string_view key,
                           const std::string_view archive_main_key) noexcept {
    return is_repository_key(key) && key.starts_with(archive_main_key) &&
           key.size() > archive_main_key.size();
}

std::string backup_type_name(const format::BackupType type) {
    switch (type) {
    case format::BackupType::kFull:
        return "full";
    case format::BackupType::kIncremental:
        return "incremental";
    case format::BackupType::kDifferential:
        return "differential";
    }
    return {};
}

base::Result<format::BackupType> parse_backup_type(const Json& value) {
    if (!value.is_string()) {
        return base::Result<format::BackupType>::failure(
            corrupt("repository backup type has invalid type"));
    }
    const auto name = value.get<std::string>();
    if (name == "full") {
        return base::Result<format::BackupType>::success(format::BackupType::kFull);
    }
    if (name == "incremental") {
        return base::Result<format::BackupType>::success(format::BackupType::kIncremental);
    }
    if (name == "differential") {
        return base::Result<format::BackupType>::success(format::BackupType::kDifferential);
    }
    return base::Result<format::BackupType>::failure(corrupt("repository backup type is invalid"));
}

} // namespace aegra::personal_repository::detail
