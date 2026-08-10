#pragma once

#include "aegra/base/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace aegra::adapters::personal_archive::detail {

struct Win32OutputStatistics final {
    std::uint64_t write_calls{0};
    std::uint64_t write_bytes{0};
    std::uint64_t write_microseconds{0};
};

class Win32OutputFile final {
  public:
    Win32OutputFile() noexcept = default;
    Win32OutputFile(const Win32OutputFile&) = delete;
    Win32OutputFile& operator=(const Win32OutputFile&) = delete;
    Win32OutputFile(Win32OutputFile&& other) noexcept;
    Win32OutputFile& operator=(Win32OutputFile&& other) noexcept;
    ~Win32OutputFile();

    [[nodiscard]] base::Result<void> open(const std::filesystem::path& path);
    [[nodiscard]] base::Result<void> write(std::span<const std::byte> bytes);
    [[nodiscard]] base::Result<void> flush() const;
    [[nodiscard]] std::uint64_t position() const noexcept;
    [[nodiscard]] Win32OutputStatistics statistics() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::uint64_t position_{0};
    Win32OutputStatistics statistics_;
};

} // namespace aegra::adapters::personal_archive::detail
