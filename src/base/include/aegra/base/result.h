#pragma once

#include "aegra/base/error.h"

#include <cassert>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace aegra::base {

template <typename T>
class [[nodiscard]] Result final {
    static_assert(!std::is_same_v<T, Error>, "Result<Error> is ambiguous");

public:
    [[nodiscard]] static Result success(T value) {
        return Result(std::move(value));
    }

    [[nodiscard]] static Result failure(Error error) {
        assert(error.is_error());
        return Result(std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] T& value() & {
        auto* stored = std::get_if<T>(&storage_);
        assert(stored != nullptr);
        return *stored;
    }

    [[nodiscard]] const T& value() const& {
        const auto* stored = std::get_if<T>(&storage_);
        assert(stored != nullptr);
        return *stored;
    }

    [[nodiscard]] T&& value() && {
        auto* stored = std::get_if<T>(&storage_);
        assert(stored != nullptr);
        return std::move(*stored);
    }

    [[nodiscard]] const Error& error() const& {
        const auto* stored = std::get_if<Error>(&storage_);
        assert(stored != nullptr);
        return *stored;
    }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> final {
public:
    [[nodiscard]] static Result success() {
        return Result();
    }

    [[nodiscard]] static Result failure(Error error) {
        assert(error.is_error());
        return Result(std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept {
        return !error_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] const Error& error() const& {
        assert(error_.has_value());
        return *error_;
    }

private:
    Result() = default;
    explicit Result(Error error) : error_(std::move(error)) {}

    std::optional<Error> error_;
};

} // namespace aegra::base
