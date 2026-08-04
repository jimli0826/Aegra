#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <optional>

namespace aegra::desktop::protocol_detail {

inline constexpr qsizetype kMaximumVersionCharacters = 64;
inline constexpr qsizetype kMaximumCapabilities = 64;
inline constexpr qsizetype kMaximumCapabilityCharacters = 64;
inline constexpr qsizetype kMaximumContinuationTokenCharacters = 1'024;

[[nodiscard]] inline bool has_exact_keys(const QJsonObject& object,
                                         const std::initializer_list<const char*> keys) {
    if (object.size() != static_cast<int>(keys.size())) {
        return false;
    }
    return std::all_of(keys.begin(), keys.end(),
                       [&object](const char* key) { return object.contains(QLatin1String(key)); });
}

[[nodiscard]] inline bool integer_in_range(const QJsonValue& value, const qint64 minimum,
                                           const qint64 maximum, qint64& result) {
    if (!value.isDouble()) {
        return false;
    }
    const auto integer = value.toInteger((std::numeric_limits<qint64>::min)());
    if (integer < minimum || integer > maximum ||
        value.toDouble() != static_cast<double>(integer)) {
        return false;
    }
    result = integer;
    return true;
}

[[nodiscard]] inline bool stable_code(const QString& value, const qsizetype maximum_characters) {
    if (value.isEmpty() || value.size() > maximum_characters) {
        return false;
    }
    return std::all_of(value.cbegin(), value.cend(), [](const QChar character) {
        const auto code = character.unicode();
        return (code >= 'a' && code <= 'z') || (code >= '0' && code <= '9') || code == '.' ||
               code == '_' || code == '-';
    });
}

[[nodiscard]] inline bool valid_message_arguments(const QJsonValue& value) {
    if (!value.isArray() || value.toArray().size() > 16) {
        return false;
    }
    QString previous;
    for (const auto& item : value.toArray()) {
        if (!item.isObject()) {
            return false;
        }
        const auto argument = item.toObject();
        if (!has_exact_keys(argument, {"name", "value"}) ||
            !argument.value(QStringLiteral("name")).isString() ||
            !stable_code(argument.value(QStringLiteral("name")).toString(), 64) ||
            !argument.value(QStringLiteral("value")).isString()) {
            return false;
        }
        const auto name = argument.value(QStringLiteral("name")).toString();
        const auto text = argument.value(QStringLiteral("value")).toString();
        if ((!previous.isEmpty() && name <= previous) || text.isEmpty() || text.size() > 256 ||
            std::any_of(text.cbegin(), text.cend(), [](const QChar character) {
                return character.unicode() < 0x20U || character.unicode() == 0x7FU;
            })) {
            return false;
        }
        previous = name;
    }
    return true;
}

[[nodiscard]] inline bool canonical_uuid(const QString& value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
        value[23] != '-') {
        return false;
    }
    for (qsizetype index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        const auto code = value[index].unicode();
        if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f'))) {
            return false;
        }
    }
    return value[14] >= '1' && value[14] <= '8' &&
           (value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
}

[[nodiscard]] inline bool optional_uuid(const QJsonValue& value, QString& result, bool& present) {
    if (value.isNull()) {
        result.clear();
        present = false;
        return true;
    }
    if (!value.isString() || !canonical_uuid(value.toString())) {
        return false;
    }
    result = value.toString();
    present = true;
    return true;
}

[[nodiscard]] inline bool parse_token(const QJsonValue& value, std::optional<QString>& result) {
    if (value.isNull()) {
        result.reset();
        return true;
    }
    if (!value.isString()) {
        return false;
    }
    const auto token = value.toString();
    if (token.isEmpty() || token.size() > kMaximumContinuationTokenCharacters ||
        !std::all_of(token.cbegin(), token.cend(), [](const QChar character) {
            return character.unicode() >= 0x21U && character.unicode() <= 0x7EU;
        })) {
        return false;
    }
    result = token;
    return true;
}

[[nodiscard]] inline bool parse_display_name(const QJsonValue& value, QString& result) {
    if (!value.isString()) {
        return false;
    }
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > 256) {
        return false;
    }
    if (std::any_of(text.cbegin(), text.cend(), [](const QChar character) {
            return character.unicode() < 0x20U || character.unicode() == 0x7FU;
        })) {
        return false;
    }
    result = text;
    return true;
}

} // namespace aegra::desktop::protocol_detail
