#pragma once

#include <utility>

namespace aegra::adapters::windows_vss::detail {

template <typename Interface> class ComPtr final {
  public:
    ComPtr() = default;
    explicit ComPtr(Interface* value) noexcept : value_(value) {}
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : value_(other.release()) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] Interface* get() const noexcept { return value_; }

    [[nodiscard]] Interface** put() noexcept {
        reset();
        return &value_;
    }

    [[nodiscard]] Interface* operator->() const noexcept { return value_; }

    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

    [[nodiscard]] Interface* release() noexcept { return std::exchange(value_, nullptr); }

    void reset(Interface* value = nullptr) noexcept {
        if (value_ != nullptr) {
            value_->Release();
        }
        value_ = value;
    }

  private:
    Interface* value_{nullptr};
};

} // namespace aegra::adapters::windows_vss::detail
