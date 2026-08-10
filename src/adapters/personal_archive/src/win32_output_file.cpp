#include "win32_output_file.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <utility>

namespace aegra::adapters::personal_archive::detail {
namespace {

[[nodiscard]] base::Error io_error(std::string message) {
    return {base::ErrorCode::kIoFailure, std::move(message)};
}

} // namespace

Win32OutputFile::Win32OutputFile(Win32OutputFile&& other) noexcept
    : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)),
      position_(std::exchange(other.position_, 0)) {}

Win32OutputFile& Win32OutputFile::operator=(Win32OutputFile&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        position_ = std::exchange(other.position_, 0);
    }
    return *this;
}

Win32OutputFile::~Win32OutputFile() { close(); }

base::Result<void> Win32OutputFile::open(const std::filesystem::path& path) {
    close();
    handle_ = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        return base::Result<void>::failure(io_error("failed to create archive output"));
    }
    position_ = 0;
    return base::Result<void>::success();
}

base::Result<void> Win32OutputFile::write(const std::span<const std::byte> bytes) {
    if (!is_open()) {
        return base::Result<void>::failure(io_error("archive output is not open"));
    }
    if (bytes.size() > (std::numeric_limits<std::uint64_t>::max)() - position_) {
        return base::Result<void>::failure(io_error("archive output position overflow"));
    }
    std::size_t written_total = 0;
    while (written_total < bytes.size()) {
        const auto remaining = bytes.size() - written_total;
        const auto request = static_cast<DWORD>((std::min)(
            remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        const auto write_start = std::chrono::steady_clock::now();
        if (::WriteFile(handle_, bytes.data() + written_total, request, &written, nullptr) == FALSE ||
            written == 0 || written > request) {
            return base::Result<void>::failure(io_error("failed to write archive output"));
        }
        statistics_.write_microseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - write_start)
                .count());
        ++statistics_.write_calls;
        statistics_.write_bytes += written;
        written_total += written;
        position_ += written;
    }
    return base::Result<void>::success();
}

base::Result<void> Win32OutputFile::flush() const {
    if (!is_open()) {
        return base::Result<void>::failure(io_error("archive output is not open"));
    }
    return base::Result<void>::success();
}

std::uint64_t Win32OutputFile::position() const noexcept { return position_; }

Win32OutputStatistics Win32OutputFile::statistics() const noexcept { return statistics_; }

bool Win32OutputFile::is_open() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

void Win32OutputFile::close() noexcept {
    if (is_open()) {
        ::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    position_ = 0;
}

} // namespace aegra::adapters::personal_archive::detail
