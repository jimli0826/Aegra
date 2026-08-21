#include "win32_input_file.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace aegra::adapters::personal_archive::detail {
namespace {

constexpr std::size_t kMaximumReadRequest = 64ULL * 1024ULL * 1024ULL;

[[nodiscard]] base::Error io_error(std::string message) {
    return {base::ErrorCode::kIoFailure, std::move(message)};
}

} // namespace

Win32InputFile::Win32InputFile(Win32InputFile&& other) noexcept
    : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)),
      position_(std::exchange(other.position_, 0)) {}

Win32InputFile& Win32InputFile::operator=(Win32InputFile&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        position_ = std::exchange(other.position_, 0);
    }
    return *this;
}

Win32InputFile::~Win32InputFile() { close(); }

base::Result<void> Win32InputFile::open(const std::filesystem::path& path) {
    close();
    handle_ = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        return base::Result<void>::failure(io_error("failed to open archive input"));
    }
    position_ = 0;
    return base::Result<void>::success();
}

base::Result<std::uint64_t> Win32InputFile::size() const {
    if (!is_open()) {
        return base::Result<std::uint64_t>::failure(io_error("archive input is not open"));
    }
    LARGE_INTEGER file_size{};
    if (::GetFileSizeEx(handle_, &file_size) == FALSE || file_size.QuadPart < 0) {
        return base::Result<std::uint64_t>::failure(io_error("failed to size archive input"));
    }
    return base::Result<std::uint64_t>::success(static_cast<std::uint64_t>(file_size.QuadPart));
}

base::Result<void> Win32InputFile::read_at(const std::uint64_t offset,
                                           const std::span<std::byte> destination) const {
    if (!is_open()) {
        return base::Result<void>::failure(io_error("archive input is not open"));
    }
    if (destination.empty()) {
        position_ = offset;
        return base::Result<void>::success();
    }
    if (offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        return base::Result<void>::failure(io_error("archive input offset is out of range"));
    }
    LARGE_INTEGER seek{};
    seek.QuadPart = static_cast<LONGLONG>(offset);
    if (::SetFilePointerEx(handle_, seek, nullptr, FILE_BEGIN) == FALSE) {
        return base::Result<void>::failure(io_error("failed to seek archive input"));
    }
    std::size_t total = 0;
    while (total < destination.size()) {
        const auto request = static_cast<DWORD>(
            (std::min)(destination.size() - total, kMaximumReadRequest));
        DWORD transferred = 0;
        if (::ReadFile(handle_, destination.data() + total, request, &transferred, nullptr) ==
                FALSE ||
            transferred == 0) {
            return base::Result<void>::failure(io_error("archive input is truncated"));
        }
        total += transferred;
    }
    position_ = offset + destination.size();
    return base::Result<void>::success();
}

base::Result<std::vector<std::byte>> Win32InputFile::read_exact_at(const std::uint64_t offset,
                                                                   const std::size_t size) const {
    if (size == 0) {
        position_ = offset;
        return base::Result<std::vector<std::byte>>::success({});
    }
    std::vector<std::byte> result(size);
    auto read = read_at(offset, result);
    if (!read) {
        return base::Result<std::vector<std::byte>>::failure(read.error());
    }
    return base::Result<std::vector<std::byte>>::success(std::move(result));
}

base::Result<void> Win32InputFile::read(const std::span<std::byte> destination) {
    return read_at(position_, destination);
}

base::Result<void> Win32InputFile::seek(const std::uint64_t offset) {
    if (!is_open()) {
        return base::Result<void>::failure(io_error("archive input is not open"));
    }
    if (offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        return base::Result<void>::failure(io_error("archive input offset is out of range"));
    }
    LARGE_INTEGER seek{};
    seek.QuadPart = static_cast<LONGLONG>(offset);
    if (::SetFilePointerEx(handle_, seek, nullptr, FILE_BEGIN) == FALSE) {
        return base::Result<void>::failure(io_error("failed to seek archive input"));
    }
    position_ = offset;
    return base::Result<void>::success();
}

std::uint64_t Win32InputFile::position() const noexcept { return position_; }

bool Win32InputFile::is_open() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

void Win32InputFile::close() noexcept {
    if (is_open()) {
        ::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    position_ = 0;
}

} // namespace aegra::adapters::personal_archive::detail
