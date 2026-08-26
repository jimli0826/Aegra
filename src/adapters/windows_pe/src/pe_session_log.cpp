#include "aegra/adapters/windows_pe/pe_session_log.h"

#include "pe_pending_internal.h"

#include <cstdio>
#include <utility>

namespace aegra::adapters::windows_pe {
namespace {

[[nodiscard]] std::string line_timestamp() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", local.wYear,
                  local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond,
                  local.wMilliseconds);
    return buffer;
}

[[nodiscard]] std::wstring file_timestamp() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    wchar_t buffer[40]{};
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%04u%02u%02u_%02u%02u%02u",
                  local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
                  local.wSecond);
    return buffer;
}

} // namespace

struct PeSessionLog::Impl final {
    detail::UniqueHandle file;
};

PeSessionLog::PeSessionLog(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

PeSessionLog::~PeSessionLog() {
    if (impl_ != nullptr && impl_->file.valid()) {
        FlushFileBuffers(impl_->file.get());
    }
}

std::unique_ptr<PeSessionLog> PeSessionLog::open(const std::string& data_dir_utf8) noexcept {
    try {
        auto data_dir = detail::utf8_to_wide(data_dir_utf8);
        if (!data_dir) {
            return nullptr;
        }
        std::wstring log_dir = std::move(data_dir).value();
        while (!log_dir.empty() && log_dir.back() == L'\\') {
            log_dir.pop_back();
        }
        log_dir += L"\\pe\\logs";
        if (auto directories = detail::ensure_directory_exists(log_dir); !directories) {
            return nullptr;
        }
        const std::wstring path = log_dir + L"\\pe_session_" + file_timestamp() + L".log";
        detail::UniqueHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                              nullptr, CREATE_ALWAYS,
                                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                              nullptr));
        if (!file.valid()) {
            return nullptr;
        }
        auto impl = std::make_unique<Impl>();
        impl->file = std::move(file);
        return std::unique_ptr<PeSessionLog>(new PeSessionLog(std::move(impl)));
    } catch (...) {
        return nullptr;
    }
}

void PeSessionLog::line(const std::string_view message) noexcept {
    if (impl_ == nullptr || !impl_->file.valid()) {
        return;
    }
    try {
        std::string text = line_timestamp();
        text += message;
        text += "\r\n";
        DWORD written = 0;
        (void)WriteFile(impl_->file.get(), text.data(), static_cast<DWORD>(text.size()), &written,
                        nullptr);
    } catch (...) {
    }
}

} // namespace aegra::adapters::windows_pe
