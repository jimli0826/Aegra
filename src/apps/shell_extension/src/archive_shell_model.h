#pragma once

#include "password_dialog.h"
#include "protected_password.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aegra::shell {

// One Explorer item produced by ArchiveShellModel (MSF HostItem identity is itemId).
struct ArchiveShellItem final {
    std::wstring itemId;
    std::wstring displayName;
    std::uint64_t size{0};
    ULONGLONG modifiedTime{0};
    std::uint32_t attributes{0};
    std::uint32_t fileAttributes{FILE_ATTRIBUTE_NORMAL};
    bool isFolder{false};
};

// Composition-root browse model for V7 .bkf (volume_set + file_set).
// Pattern matches msf-main msf_host / backup BackupShellModel; readers are Aegra adapters only.
// COM DLL lifetime holds the singleton; no password material is stored in item IDs.
class ArchiveShellModel final {
  public:
    static ArchiveShellModel& Instance();
    ~ArchiveShellModel();

    ArchiveShellModel(const ArchiveShellModel&) = delete;
    ArchiveShellModel& operator=(const ArchiveShellModel&) = delete;

    /// Authenticate and open the archive before Explorer creates its folder view.
    /// ERROR_CANCELLED means navigation must remain at the current Explorer location.
    HRESULT PrepareFolderView(const std::wstring& rootPath);
    HRESULT EnumChildren(const std::wstring& rootPath, const std::wstring& folderId,
                         std::vector<ArchiveShellItem>& items);
    HRESULT GetItemInfo(const std::wstring& rootPath, const std::wstring& itemId,
                        ArchiveShellItem& item);
    HRESULT GetInfoTip(const std::wstring& rootPath, const std::wstring& itemId,
                       std::wstring& text);
    HRESULT OpenItem(const std::wstring& rootPath, HWND hwnd, const std::wstring& itemId);

    /// Streaming read for Explorer copy (CFSTR_FILECONTENTS / IStream).
    HRESULT ReadFileData(const std::wstring& rootPath, const std::wstring& itemId,
                         ULONGLONG offset, void* buffer, DWORD length, DWORD* bytesRead);

  private:
    enum class ContentKind : std::uint8_t {
        kUnknown = 0,
        kVolumeSet = 1,
        kFileSet = 2,
    };

    struct VolumeState final {
        std::uint32_t disk_number{0};
        std::uint32_t volume_index{0};
        std::wstring display_name;
        std::wstring root_item_id;
        std::string filesystem_utf8;
        std::unique_ptr<class VolumeBrowseContext> browse;
        bool browse_attempted{false};
        HRESULT browse_result{E_FAIL};
    };

    struct ArchiveSession final {
        std::wstring root_path;
        ContentKind content_kind{ContentKind::kUnknown};
        std::unique_ptr<class VolumeSetContext> volume_set;
        std::unique_ptr<class FileSetContext> file_set;
        std::vector<VolumeState> volumes;
        std::wstring error;
        bool opened{false};
        /// Sticky permanent open failure (parent_missing, corrupt, invalid path).
        bool open_failed{false};
        HRESULT open_hr{E_FAIL};
        /// True while open/password UI runs. Blocks nested prompts on re-entrant Explorer calls.
        bool opening{false};
        /// Session-local DPAPI ciphertext. Cleared on cancel/sticky failure and destruction.
        ProtectedPassword credential;
    };

    ArchiveShellModel() = default;

    ArchiveSession& GetSessionLocked(const std::wstring& rootPath);
    [[nodiscard]] bool DecryptSessionCredential(ArchiveSession& session,
                                                SecurePassword& plaintext);
    void ProtectSessionCredential(ArchiveSession& session, std::string_view plaintext);
    /// Holds the model mutex for the whole open, including the password dialog. Password
    /// validation occurs inside one modal dialog, matching the AIVImage ShellFolder flow.
    HRESULT EnsureSessionOpened(ArchiveSession& session);
    HRESULT OpenSessionWithCredential(ArchiveSession& session, std::string_view password);
    HRESULT EnsureVolumeBrowseLocked(ArchiveSession& session, VolumeState& volume);

    HRESULT EnumRootDisks(const ArchiveSession& session, std::vector<ArchiveShellItem>& items);
    HRESULT EnumDiskVolumes(const ArchiveSession& session, std::uint32_t disk_number,
                            std::vector<ArchiveShellItem>& items);
    HRESULT EnumNtfsChildren(ArchiveSession& session, const std::wstring& folderId,
                             std::vector<ArchiveShellItem>& items);
    HRESULT EnumFileSetChildren(ArchiveSession& session, const std::wstring& folderId,
                                std::vector<ArchiveShellItem>& items);

    HRESULT GetVolumeSetItemInfo(ArchiveSession& session, const std::wstring& itemId,
                                 ArchiveShellItem& item);
    HRESULT GetFileSetItemInfo(ArchiveSession& session, const std::wstring& itemId,
                               ArchiveShellItem& item);
    HRESULT OpenNtfsFile(ArchiveSession& session, HWND hwnd, const std::wstring& itemId);
    HRESULT OpenFileSetFile(ArchiveSession& session, HWND hwnd, const std::wstring& itemId);
    HRESULT ReadNtfsFileDataLocked(ArchiveSession& session, const std::wstring& itemId,
                                   ULONGLONG offset, void* buffer, DWORD length,
                                   DWORD* bytesRead);
    HRESULT ReadFileSetDataLocked(ArchiveSession& session, const std::wstring& itemId,
                                  ULONGLONG offset, void* buffer, DWORD length,
                                  DWORD* bytesRead);

    /// recursive: DialogBox message pump can re-enter shell folder methods on the same STA thread.
    std::recursive_mutex mutex_;
    std::unordered_map<std::wstring, std::unique_ptr<ArchiveSession>> sessions_;
};

} // namespace aegra::shell
