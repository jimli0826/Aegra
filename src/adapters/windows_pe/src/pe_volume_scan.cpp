#include "aegra/adapters/windows_pe/pe_pending_store.h"

#include "pe_pending_internal.h"

#include <utility>
#include <vector>

namespace aegra::adapters::windows_pe {
namespace {

class VolumeIterator final {
  public:
    VolumeIterator() noexcept = default;
    ~VolumeIterator() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            FindVolumeClose(handle_);
        }
    }
    VolumeIterator(const VolumeIterator&) = delete;
    VolumeIterator& operator=(const VolumeIterator&) = delete;
    VolumeIterator(VolumeIterator&&) = delete;
    VolumeIterator& operator=(VolumeIterator&&) = delete;

    /// Returns false when the enumeration is exhausted; failure_code() tells
    /// a clean end (ERROR_NO_MORE_FILES → 0) apart from a real failure.
    [[nodiscard]] bool next(std::wstring& volume_guid_path) {
        wchar_t buffer[MAX_PATH]{};
        if (handle_ == INVALID_HANDLE_VALUE) {
            handle_ = FindFirstVolumeW(buffer, MAX_PATH);
            if (handle_ == INVALID_HANDLE_VALUE) {
                record_failure();
                return false;
            }
        } else if (!FindNextVolumeW(handle_, buffer, MAX_PATH)) {
            record_failure();
            return false;
        }
        volume_guid_path.assign(buffer);
        return true;
    }

    /// 0 after a clean end of enumeration; the Win32 error code otherwise.
    [[nodiscard]] DWORD failure_code() const noexcept { return failure_code_; }

  private:
    void record_failure() noexcept {
        const auto code = GetLastError();
        failure_code_ = code == ERROR_NO_MORE_FILES ? 0 : code;
    }

    HANDLE handle_{INVALID_HANDLE_VALUE};
    DWORD failure_code_{0};
};

struct VolumeMatch final {
    /// `<volume>\ProgramData\Aegra` — the host data directory.
    std::wstring data_directory;
    /// `<volume>\ProgramData\Aegra\pe\pending` — the pending store directory.
    std::wstring pending_directory;
};

/// A drive-letter mount point ("C:\") for the volume, or empty when the volume is
/// not mounted to a letter. The volume-GUID form works with Win32 CreateFileW but
/// NOT with the CRT fopen that spdlog (the worker's task log) uses, so a
/// drive-letter base is required for the worker's AEGRA_DATA_DIR to be writable.
[[nodiscard]] std::wstring drive_letter_mount(const std::wstring& volume_guid_path) {
    DWORD length = 0;
    GetVolumePathNamesForVolumeNameW(volume_guid_path.c_str(), nullptr, 0, &length);
    if (length == 0) {
        return {};
    }
    std::vector<wchar_t> buffer(length);
    if (!GetVolumePathNamesForVolumeNameW(volume_guid_path.c_str(), buffer.data(), length,
                                          &length)) {
        return {};
    }
    for (const wchar_t* cursor = buffer.data(); *cursor != L'\0'; cursor += wcslen(cursor) + 1) {
        std::wstring mount(cursor);
        // "C:\" — a drive-letter root, not a folder mount point.
        if (mount.size() == 3 && mount[1] == L':' && mount[2] == L'\\') {
            return mount;
        }
    }
    return {};
}

/// Base directory `<root>\ProgramData\Aegra`. Prefers the drive-letter mount so the
/// worker's CRT-based log can be written; falls back to the volume-GUID root.
[[nodiscard]] std::wstring data_directory_on_volume(const std::wstring& volume_guid_path) {
    std::wstring root = drive_letter_mount(volume_guid_path);
    if (root.empty()) {
        root = volume_guid_path; // volume GUID path carries a trailing slash
    }
    std::wstring directory = root;
    directory += detail::kDefaultDataDirRelative;
    return directory;
}

} // namespace

base::Result<LocatedPePendingStore> locate_pe_pending_store() {
    using Output = base::Result<LocatedPePendingStore>;
    std::vector<VolumeMatch> matches;
    VolumeIterator volumes;
    std::wstring volume_guid_path;
    while (volumes.next(volume_guid_path)) {
        if (GetDriveTypeW(volume_guid_path.c_str()) != DRIVE_FIXED) {
            continue;
        }
        const auto data_directory = data_directory_on_volume(volume_guid_path);
        std::wstring pending_directory = data_directory + L'\\';
        pending_directory += detail::kPendingDirRelative;
        const auto job_path =
            pending_directory + L"\\" + std::wstring(detail::kPendingJobFileName);
        if (detail::file_exists(job_path)) {
            matches.push_back({data_directory, std::move(pending_directory)});
        }
    }
    if (volumes.failure_code() != 0) {
        return Output::failure(detail::win32_error(volumes.failure_code(), "enumerate volumes"));
    }
    if (matches.empty()) {
        return Output::failure(detail::pe_store_error(
            base::ErrorCode::kNotFound, "no volume holds a pe pending job"));
    }
    if (matches.size() > 1) {
        return Output::failure(detail::pe_store_error(
            base::ErrorCode::kConflict,
            "multiple volumes hold a pe pending job; refusing to guess"));
    }
    auto data_dir_utf8 = detail::wide_to_utf8(matches.front().data_directory);
    if (!data_dir_utf8) {
        return Output::failure(data_dir_utf8.error());
    }
    LocatedPePendingStore located;
    located.store =
        detail::make_pe_pending_store_for_directory(std::move(matches.front().pending_directory));
    located.data_dir_utf8 = std::move(data_dir_utf8).value();
    return Output::success(std::move(located));
}

} // namespace aegra::adapters::windows_pe
