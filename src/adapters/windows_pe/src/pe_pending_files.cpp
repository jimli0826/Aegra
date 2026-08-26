#include "pe_pending_internal.h"

#include <sddl.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace aegra::adapters::windows_pe::detail {
namespace {

/// SYSTEM and Administrators full access only; no inheritance (ADR-0026 B3).
inline constexpr const wchar_t* kKeyFileSddl = L"D:PAI(A;;FA;;;SY)(A;;FA;;;BA)";

class LocalSecurityDescriptor final {
  public:
    LocalSecurityDescriptor() noexcept = default;
    ~LocalSecurityDescriptor() {
        if (descriptor_ != nullptr) {
            LocalFree(descriptor_);
        }
    }
    LocalSecurityDescriptor(const LocalSecurityDescriptor&) = delete;
    LocalSecurityDescriptor& operator=(const LocalSecurityDescriptor&) = delete;
    LocalSecurityDescriptor(LocalSecurityDescriptor&&) = delete;
    LocalSecurityDescriptor& operator=(LocalSecurityDescriptor&&) = delete;

    [[nodiscard]] PSECURITY_DESCRIPTOR* receive() noexcept { return &descriptor_; }
    [[nodiscard]] PSECURITY_DESCRIPTOR get() const noexcept { return descriptor_; }

  private:
    PSECURITY_DESCRIPTOR descriptor_{nullptr};
};

[[nodiscard]] base::Result<void> write_all(const HANDLE handle,
                                           const std::span<const std::byte> content,
                                           const char* operation) {
    std::size_t written = 0;
    while (written < content.size()) {
        const auto remaining = content.size() - written;
        const auto requested = static_cast<DWORD>(
            (std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD actual = 0;
        if (!WriteFile(handle, content.data() + written, requested, &actual, nullptr) ||
            actual == 0) {
            return base::Result<void>::failure(win32_error(GetLastError(), operation));
        }
        written += actual;
    }
    return base::Result<void>::success();
}

/// First character index after the root component (`C:\`, `\\?\C:\`, `\\?\Volume{...}\`).
[[nodiscard]] std::size_t directory_walk_start(const std::wstring& directory) {
    if (directory.starts_with(LR"(\\?\)")) {
        const auto separator = directory.find(L'\\', 4);
        return separator == std::wstring::npos ? directory.size() : separator + 1;
    }
    if (directory.size() >= 3 && directory[1] == L':' && directory[2] == L'\\') {
        return 3;
    }
    return 0;
}

} // namespace

base::Error pe_store_error(const base::ErrorCode code, const char* message) {
    return {code, message};
}

base::Error win32_error(const DWORD code, const char* operation) {
    std::string message = "pe pending store: ";
    message += operation;
    message += " failed with win32 error ";
    message += std::to_string(code);
    return {base::ErrorCode::kIoFailure, std::move(message)};
}

base::Result<std::wstring> utf8_to_wide(const std::string_view value) {
    if (value.empty()) {
        return base::Result<std::wstring>::success(std::wstring());
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return base::Result<std::wstring>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "path is too long"));
    }
    const auto input_size = static_cast<int>(value.size());
    const auto required =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0);
    if (required <= 0) {
        return base::Result<std::wstring>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "path is not valid utf-8"));
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, wide.data(),
                            required) != required) {
        return base::Result<std::wstring>::failure(
            win32_error(GetLastError(), "convert utf-8 path"));
    }
    return base::Result<std::wstring>::success(std::move(wide));
}

base::Result<std::string> wide_to_utf8(const std::wstring_view value) {
    if (value.empty()) {
        return base::Result<std::string>::success(std::string());
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return base::Result<std::string>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "path is too long"));
    }
    const auto input_size = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                              input_size, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return base::Result<std::string>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "path is not valid utf-16"));
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size, utf8.data(),
                            required, nullptr, nullptr) != required) {
        return base::Result<std::string>::failure(win32_error(GetLastError(), "convert path"));
    }
    return base::Result<std::string>::success(std::move(utf8));
}

base::Result<std::vector<std::byte>> read_bounded_file(const std::wstring& path,
                                                       const std::size_t maximum_size) {
    UniqueHandle handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        const auto code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return base::Result<std::vector<std::byte>>::failure(
                pe_store_error(base::ErrorCode::kNotFound, "pe pending document not found"));
        }
        return base::Result<std::vector<std::byte>>::failure(
            win32_error(code, "open pending document"));
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size)) {
        return base::Result<std::vector<std::byte>>::failure(
            win32_error(GetLastError(), "measure pending document"));
    }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > maximum_size) {
        return base::Result<std::vector<std::byte>>::failure(
            pe_store_error(base::ErrorCode::kCorruptData, "pe pending document size is invalid"));
    }
    std::vector<std::byte> content(static_cast<std::size_t>(size.QuadPart));
    std::size_t consumed = 0;
    while (consumed < content.size()) {
        DWORD actual = 0;
        const auto requested = static_cast<DWORD>((std::min)(
            content.size() - consumed, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        if (!ReadFile(handle.get(), content.data() + consumed, requested, &actual, nullptr) ||
            actual == 0) {
            return base::Result<std::vector<std::byte>>::failure(
                win32_error(GetLastError(), "read pending document"));
        }
        consumed += actual;
    }
    return base::Result<std::vector<std::byte>>::success(std::move(content));
}

base::Result<void> write_document_atomically(const std::wstring& path,
                                             const std::string_view content,
                                             const bool replace_existing) {
    const std::wstring temporary = path + L".tmp";
    {
        UniqueHandle handle(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!handle.valid()) {
            return base::Result<void>::failure(
                win32_error(GetLastError(), "create pending document"));
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) byte view of UTF-8 text.
        const auto* bytes = reinterpret_cast<const std::byte*>(content.data());
        if (auto written = write_all(handle.get(), {bytes, content.size()},
                                     "write pending document");
            !written) {
            handle.reset();
            DeleteFileW(temporary.c_str());
            return written;
        }
        if (!FlushFileBuffers(handle.get())) {
            const auto code = GetLastError();
            handle.reset();
            DeleteFileW(temporary.c_str());
            return base::Result<void>::failure(win32_error(code, "flush pending document"));
        }
    }
    const DWORD move_flags =
        MOVEFILE_WRITE_THROUGH | (replace_existing ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(temporary.c_str(), path.c_str(), move_flags)) {
        const auto code = GetLastError();
        DeleteFileW(temporary.c_str());
        if (!replace_existing && code == ERROR_ALREADY_EXISTS) {
            return base::Result<void>::failure(
                pe_store_error(base::ErrorCode::kConflict, "pe pending document already exists"));
        }
        return base::Result<void>::failure(win32_error(code, "publish pending document"));
    }
    return base::Result<void>::success();
}

base::Result<void> write_key_file(const std::wstring& path,
                                  const std::span<const std::byte> job_key) {
    LocalSecurityDescriptor descriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kKeyFileSddl, SDDL_REVISION_1, descriptor.receive(), nullptr)) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "build key file security descriptor"));
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor.get();
    attributes.bInheritHandle = FALSE;
    UniqueHandle handle(CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        const auto code = GetLastError();
        if (code == ERROR_FILE_EXISTS) {
            return base::Result<void>::failure(
                pe_store_error(base::ErrorCode::kConflict, "pe job key file already exists"));
        }
        return base::Result<void>::failure(win32_error(code, "create key file"));
    }
    if (auto written = write_all(handle.get(), job_key, "write key file"); !written) {
        handle.reset();
        DeleteFileW(path.c_str());
        return written;
    }
    if (!FlushFileBuffers(handle.get())) {
        const auto code = GetLastError();
        handle.reset();
        DeleteFileW(path.c_str());
        return base::Result<void>::failure(win32_error(code, "flush key file"));
    }
    return base::Result<void>::success();
}

base::Result<void> scrub_and_delete_key_file(const std::wstring& path) {
    {
        UniqueHandle handle(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!handle.valid()) {
            const auto code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                return base::Result<void>::success();
            }
            return base::Result<void>::failure(win32_error(code, "open key file for scrub"));
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle.get(), &size)) {
            return base::Result<void>::failure(win32_error(GetLastError(), "measure key file"));
        }
        // A valid key file is 32 bytes; cap defensively so a corrupt size cannot stall us.
        const auto scrub_size =
            static_cast<std::size_t>((std::min)(size.QuadPart, static_cast<LONGLONG>(4096)));
        const std::vector<std::byte> zeros(scrub_size, std::byte{0});
        if (auto scrubbed = write_all(handle.get(), zeros, "scrub key file"); !scrubbed) {
            return scrubbed;
        }
        if (!FlushFileBuffers(handle.get())) {
            return base::Result<void>::failure(win32_error(GetLastError(), "flush key scrub"));
        }
    }
    return delete_file_if_exists(path);
}

base::Result<void> delete_file_if_exists(const std::wstring& path) {
    if (DeleteFileW(path.c_str())) {
        return base::Result<void>::success();
    }
    const auto code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        return base::Result<void>::success();
    }
    return base::Result<void>::failure(win32_error(code, "delete pending file"));
}

bool file_exists(const std::wstring& path) noexcept {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

base::Result<void> ensure_directory_exists(const std::wstring& directory) {
    const auto start = directory_walk_start(directory);
    if (start >= directory.size()) {
        return base::Result<void>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "data directory path is invalid"));
    }
    std::size_t position = start;
    while (position <= directory.size()) {
        const auto separator = directory.find(L'\\', position);
        const auto component_end = separator == std::wstring::npos ? directory.size() : separator;
        if (component_end > position) {
            const std::wstring prefix = directory.substr(0, component_end);
            if (!CreateDirectoryW(prefix.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                return base::Result<void>::failure(
                    win32_error(GetLastError(), "create pending directory"));
            }
        }
        if (separator == std::wstring::npos) {
            break;
        }
        position = separator + 1;
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::windows_pe::detail
