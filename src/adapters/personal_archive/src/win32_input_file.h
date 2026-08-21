#pragma once

#include "aegra/base/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace aegra::adapters::personal_archive::detail {

class Win32InputFile final {
  public:
    Win32InputFile() noexcept = default;
    Win32InputFile(const Win32InputFile&) = delete;
    Win32InputFile& operator=(const Win32InputFile&) = delete;
    Win32InputFile(Win32InputFile&& other) noexcept;
    Win32InputFile& operator=(Win32InputFile&& other) noexcept;
    ~Win32InputFile();

    [[nodiscard]] base::Result<void> open(const std::filesystem::path& path);
    [[nodiscard]] base::Result<std::uint64_t> size() const;
    [[nodiscard]] base::Result<void> read_at(std::uint64_t offset,
                                             std::span<std::byte> destination) const;
    [[nodiscard]] base::Result<std::vector<std::byte>> read_exact_at(std::uint64_t offset,
                                                                     std::size_t size) const;
    [[nodiscard]] base::Result<void> read(std::span<std::byte> destination);
    [[nodiscard]] base::Result<void> seek(std::uint64_t offset);
    [[nodiscard]] std::uint64_t position() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    mutable std::uint64_t position_{0};
};

} // namespace aegra::adapters::personal_archive::detail
