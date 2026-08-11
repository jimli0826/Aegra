#pragma once

// Explorer copy/paste and drag-drop for virtual .bkf items.
// Provides CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS (IStream) via MSF ShellFolderDataObjectImpl.

#include "archive_shell_model.h"
#include "host_item.h"

#include "aegra/adapters/ntfs/ntfs_reader.h"

#include <msf.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace aegra::shell {
namespace {

[[nodiscard]] std::wstring sanitize_copy_relative_path(std::wstring path) {
    for (auto& ch : path) {
        if (ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'/' || ch == L'|' ||
            ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    while (!path.empty() && (path.front() == L'\\' || path.front() == L'.')) {
        path.erase(path.begin());
    }
    return path.empty() ? L"item" : path;
}

[[nodiscard]] std::wstring copy_child_path(const std::wstring& parent, const std::wstring& name) {
    if (parent.empty()) {
        return sanitize_copy_relative_path(name);
    }
    return parent + L"\\" + sanitize_copy_relative_path(name);
}

} // namespace

struct ArchiveCopyItem final {
    std::wstring itemId;
    std::wstring relativePath;
    std::uint64_t size{0};
    ULONGLONG modifiedTime{0};
    std::uint32_t fileAttributes{FILE_ATTRIBUTE_NORMAL};
    bool isFolder{false};
};

// Lazily expands selected items (and folder trees) into FILEGROUPDESCRIPTOR entries.
class ArchiveCopyState final {
  public:
    ArchiveCopyState(std::wstring rootPath, std::vector<std::wstring> itemIds)
        : root_path_(std::move(rootPath)), item_ids_(std::move(itemIds)) {}

    [[nodiscard]] const std::wstring& RootPath() const noexcept { return root_path_; }

    [[nodiscard]] const std::vector<ArchiveCopyItem>& Items() {
        EnsureBuilt();
        return copy_items_;
    }

    [[nodiscard]] bool IsPotentialIndex(const LONG index) const noexcept { return index >= 0; }

  private:
    void EnsureBuilt() {
        if (built_) {
            return;
        }
        copy_items_.clear();
        for (const auto& item_id : item_ids_) {
            ArchiveShellItem item;
            if (FAILED(ArchiveShellModel::Instance().GetItemInfo(root_path_, item_id, item))) {
                continue;
            }
            AddCopyItemRecursive(item, sanitize_copy_relative_path(item.displayName));
        }
        built_ = true;
    }

    void AddCopyItemRecursive(const ArchiveShellItem& item, const std::wstring& relative_path) {
        ArchiveCopyItem copy_item;
        copy_item.itemId = item.itemId;
        copy_item.relativePath = relative_path;
        copy_item.size = item.size;
        copy_item.modifiedTime = item.modifiedTime;
        copy_item.fileAttributes = item.fileAttributes;
        copy_item.isFolder = item.isFolder;
        copy_items_.push_back(std::move(copy_item));

        if (!item.isFolder) {
            return;
        }
        std::vector<ArchiveShellItem> children;
        if (FAILED(ArchiveShellModel::Instance().EnumChildren(root_path_, item.itemId, children))) {
            return;
        }
        for (const auto& child : children) {
            AddCopyItemRecursive(child, copy_child_path(relative_path, child.displayName));
        }
    }

    std::wstring root_path_;
    std::vector<std::wstring> item_ids_;
    bool built_{false};
    std::vector<ArchiveCopyItem> copy_items_;
};

// Read-only IStream over ArchiveShellModel::ReadFileData.
class ArchiveFileContentStream final : public IStream {
  public:
    ArchiveFileContentStream(std::wstring rootPath, std::wstring itemId)
        : ref_count_(1), root_path_(std::move(rootPath)), item_id_(std::move(itemId)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream) {
            *ppv = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&ref_count_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = static_cast<ULONG>(::InterlockedDecrement(&ref_count_));
        if (count == 0) {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        if (pcbRead != nullptr) {
            *pcbRead = 0;
        }
        if (pv == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        const HRESULT hr = EnsureOpen();
        if (FAILED(hr)) {
            return hr;
        }
        if (position_ >= file_size_ || cb == 0) {
            return cb == 0 ? S_OK : S_FALSE;
        }
        constexpr ULONG kMaxChunk = adapters::ntfs::kMaximumStreamReadBytes;
        auto* out = static_cast<std::byte*>(pv);
        ULONG total_read = 0;
        while (total_read < cb && position_ < file_size_) {
            const ULONGLONG remaining_file = file_size_ - position_;
            const ULONG remaining_req = cb - total_read;
            const ULONGLONG capped_req = (std::min)(static_cast<ULONGLONG>(remaining_req),
                                                    static_cast<ULONGLONG>(kMaxChunk));
            const ULONG to_read = static_cast<ULONG>((std::min)(capped_req, remaining_file));
            DWORD bytes_read = 0;
            const HRESULT read_hr = ArchiveShellModel::Instance().ReadFileData(
                root_path_, item_id_, position_, out + total_read, to_read, &bytes_read);
            if (FAILED(read_hr)) {
                if (pcbRead != nullptr) {
                    *pcbRead = total_read;
                }
                return read_hr;
            }
            if (bytes_read > to_read) {
                if (pcbRead != nullptr) {
                    *pcbRead = total_read;
                }
                return STG_E_READFAULT;
            }
            if (bytes_read == 0) {
                break;
            }
            position_ += bytes_read;
            total_read += bytes_read;
            if (bytes_read < to_read) {
                break;
            }
        }
        if (pcbRead != nullptr) {
            *pcbRead = total_read;
        }
        return total_read == cb ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
        return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin,
                                   ULARGE_INTEGER* plibNewPosition) override {
        const HRESULT hr = EnsureOpen();
        if (FAILED(hr)) {
            return hr;
        }
        LONGLONG next = 0;
        switch (dwOrigin) {
        case STREAM_SEEK_SET:
            next = dlibMove.QuadPart;
            break;
        case STREAM_SEEK_CUR:
            next = static_cast<LONGLONG>(position_) + dlibMove.QuadPart;
            break;
        case STREAM_SEEK_END:
            next = static_cast<LONGLONG>(file_size_) + dlibMove.QuadPart;
            break;
        default:
            return STG_E_INVALIDFUNCTION;
        }
        if (next < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        position_ = static_cast<ULONGLONG>(next);
        if (plibNewPosition != nullptr) {
            plibNewPosition->QuadPart = position_;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }

    HRESULT STDMETHODCALLTYPE CopyTo(IStream* pstm, ULARGE_INTEGER cb, ULARGE_INTEGER* pcbRead,
                                     ULARGE_INTEGER* pcbWritten) override {
        if (pcbRead != nullptr) {
            pcbRead->QuadPart = 0;
        }
        if (pcbWritten != nullptr) {
            pcbWritten->QuadPart = 0;
        }
        if (pstm == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        std::vector<BYTE> buffer(1024 * 1024);
        ULONGLONG remaining = cb.QuadPart;
        while (remaining > 0) {
            ULONG read = 0;
            const ULONG to_read =
                static_cast<ULONG>((std::min)(remaining, static_cast<ULONGLONG>(buffer.size())));
            HRESULT hr = Read(buffer.data(), to_read, &read);
            if (FAILED(hr)) {
                return hr;
            }
            if (read == 0) {
                break;
            }
            ULONG written = 0;
            hr = pstm->Write(buffer.data(), read, &written);
            if (FAILED(hr)) {
                return hr;
            }
            if (pcbRead != nullptr) {
                pcbRead->QuadPart += read;
            }
            if (pcbWritten != nullptr) {
                pcbWritten->QuadPart += written;
            }
            remaining -= read;
            if (written != read) {
                return STG_E_WRITEFAULT;
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_REVERTED; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD grfStatFlag) override {
        if (pstatstg == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        const HRESULT hr = EnsureOpen();
        if (FAILED(hr)) {
            return hr;
        }
        ZeroMemory(pstatstg, sizeof(*pstatstg));
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart = file_size_;
        pstatstg->mtime = modified_time_;
        pstatstg->ctime = modified_time_;
        pstatstg->atime = modified_time_;
        pstatstg->grfMode = STGM_READ;
        if ((grfStatFlag & STATFLAG_NONAME) == 0) {
            const size_t bytes = (file_name_.size() + 1) * sizeof(wchar_t);
            pstatstg->pwcsName = static_cast<LPOLESTR>(::CoTaskMemAlloc(bytes));
            if (pstatstg->pwcsName == nullptr) {
                return E_OUTOFMEMORY;
            }
            wcscpy_s(pstatstg->pwcsName, file_name_.size() + 1, file_name_.c_str());
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream** ppstm) override {
        if (ppstm == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        auto* stream = new (std::nothrow) ArchiveFileContentStream(root_path_, item_id_);
        if (stream == nullptr) {
            return E_OUTOFMEMORY;
        }
        stream->position_ = position_;
        *ppstm = stream;
        return S_OK;
    }

  private:
    HRESULT EnsureOpen() {
        if (opened_) {
            return S_OK;
        }
        ArchiveShellItem item;
        const HRESULT info_hr =
            ArchiveShellModel::Instance().GetItemInfo(root_path_, item_id_, item);
        if (FAILED(info_hr)) {
            return info_hr;
        }
        if (item.isFolder) {
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
        file_size_ = item.size;
        file_name_ = item.displayName;
        modified_time_.dwLowDateTime = static_cast<DWORD>(item.modifiedTime & 0xFFFFFFFFu);
        modified_time_.dwHighDateTime = static_cast<DWORD>(item.modifiedTime >> 32);
        opened_ = true;
        return S_OK;
    }

    long ref_count_;
    std::wstring root_path_;
    std::wstring item_id_;
    ULONGLONG position_{0};
    ULONGLONG file_size_{0};
    std::wstring file_name_;
    FILETIME modified_time_{};
    bool opened_{false};
};

class ArchiveFileDescriptorHandler final : public msf::ClipboardFormatHandler {
  public:
    ArchiveFileDescriptorHandler(std::shared_ptr<ArchiveCopyState> state, IDataObject* dataObject)
        : ClipboardFormatHandler(CFSTR_FILEDESCRIPTORW, true, false), state_(std::move(state)),
          data_object_(dataObject) {}

    void GetData(const FORMATETC&, STGMEDIUM& storageMedium) const override {
        // Reject early QueryGetData-style probes before async copy starts (same as backup).
        if (data_object_ != nullptr) {
            ATL::CComPtr<IDataObjectAsyncCapability> async_operation;
            if (SUCCEEDED(data_object_->QueryInterface(
                    IID_IDataObjectAsyncCapability, reinterpret_cast<void**>(&async_operation))) &&
                async_operation != nullptr) {
                BOOL in_operation = FALSE;
                if (SUCCEEDED(async_operation->InOperation(&in_operation)) && !in_operation) {
                    msf::RaiseException(DV_E_FORMATETC);
                }
            }
        }

        const auto& items = state_->Items();
        const size_t descriptor_bytes =
            sizeof(FILEGROUPDESCRIPTORW) +
            (items.empty() ? 0 : (items.size() - 1) * sizeof(FILEDESCRIPTORW));
        HGLOBAL global = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, descriptor_bytes);
        msf::RaiseExceptionIf(!global, E_OUTOFMEMORY);

        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(::GlobalLock(global));
        if (group == nullptr) {
            ::GlobalFree(global);
            msf::RaiseException(HRESULT_FROM_WIN32(::GetLastError()));
        }

        group->cItems = static_cast<UINT>(items.size());
        for (UINT index = 0; index < group->cItems; ++index) {
            const ArchiveCopyItem& item = items[index];
            FILEDESCRIPTORW& descriptor = group->fgd[index];
            ZeroMemory(&descriptor, sizeof(descriptor));
            descriptor.dwFlags = FD_ATTRIBUTES | FD_WRITESTIME | FD_PROGRESSUI;
            descriptor.dwFileAttributes =
                item.fileAttributes == 0 ? FILE_ATTRIBUTE_NORMAL : item.fileAttributes;
            if (item.isFolder) {
                descriptor.dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
            } else {
                descriptor.dwFlags |= FD_FILESIZE;
                descriptor.nFileSizeHigh = static_cast<DWORD>(item.size >> 32);
                descriptor.nFileSizeLow = static_cast<DWORD>(item.size & 0xFFFFFFFFu);
            }
            descriptor.ftLastWriteTime.dwLowDateTime =
                static_cast<DWORD>(item.modifiedTime & 0xFFFFFFFFu);
            descriptor.ftLastWriteTime.dwHighDateTime = static_cast<DWORD>(item.modifiedTime >> 32);
            wcsncpy_s(descriptor.cFileName, item.relativePath.c_str(), _TRUNCATE);
        }
        ::GlobalUnlock(global);
        msf::StorageMedium::SetHGlobal(storageMedium, global);
    }

  private:
    std::shared_ptr<ArchiveCopyState> state_;
    IDataObject* data_object_;
};

class ArchiveFileContentsHandler final : public msf::ClipboardFormatHandler {
  public:
    explicit ArchiveFileContentsHandler(std::shared_ptr<ArchiveCopyState> state)
        : ClipboardFormatHandler(CFSTR_FILECONTENTS, true, false), state_(std::move(state)) {}

    HRESULT Validate(const FORMATETC& formatEtc) const noexcept override {
        if (formatEtc.dwAspect != DVASPECT_CONTENT) {
            return DV_E_DVASPECT;
        }
        if (!msf::IsBitSet(formatEtc.tymed, TYMED_ISTREAM)) {
            return DV_E_TYMED;
        }
        if (!state_->IsPotentialIndex(formatEtc.lindex)) {
            return DV_E_LINDEX;
        }
        return S_OK;
    }

    void GetData(const FORMATETC& formatEtc, STGMEDIUM& storageMedium) const override {
        const auto& items = state_->Items();
        if (formatEtc.lindex < 0 || static_cast<size_t>(formatEtc.lindex) >= items.size()) {
            msf::RaiseException(DV_E_LINDEX);
        }
        const ArchiveCopyItem& item = items[static_cast<size_t>(formatEtc.lindex)];
        if (item.isFolder) {
            msf::RaiseException(DV_E_FORMATETC);
        }
        auto* stream = new (std::nothrow) ArchiveFileContentStream(state_->RootPath(), item.itemId);
        msf::RaiseExceptionIf(!stream, E_OUTOFMEMORY);
        storageMedium.tymed = TYMED_ISTREAM;
        storageMedium.pstm = stream;
        storageMedium.pUnkForRelease = nullptr;
    }

  private:
    std::shared_ptr<ArchiveCopyState> state_;
};

// Vendored msf-main ShellFolderDataObjectImpl is IDataObject-only; async is required
// so Explorer can copy virtual files without blocking the UI thread.
class __declspec(novtable) ArchiveShellDataObject
    : public ATL::CComObjectRootEx<ATL::CComSingleThreadModel>,
      public msf::ShellFolderDataObjectImpl<ArchiveShellDataObject>,
      public IDataObjectAsyncCapability {
  public:
    BEGIN_COM_MAP(ArchiveShellDataObject)
    COM_INTERFACE_ENTRY(IDataObject)
    COM_INTERFACE_ENTRY(IDataObjectAsyncCapability)
    END_COM_MAP()

    DECLARE_NOT_AGGREGATABLE(ArchiveShellDataObject)

    HRESULT __stdcall SetAsyncMode(BOOL fDoOpAsync) noexcept override {
        do_async_ = fDoOpAsync;
        return S_OK;
    }

    HRESULT __stdcall GetAsyncMode(BOOL* pfIsOpAsync) noexcept override {
        if (pfIsOpAsync == nullptr) {
            return E_POINTER;
        }
        *pfIsOpAsync = TRUE;
        return S_OK;
    }

    HRESULT __stdcall StartOperation(IBindCtx* /*pbcReserved*/) noexcept override {
        in_operation_ = TRUE;
        return S_OK;
    }

    HRESULT __stdcall InOperation(BOOL* pfInAsyncOp) noexcept override {
        if (pfInAsyncOp == nullptr) {
            return E_POINTER;
        }
        *pfInAsyncOp = in_operation_;
        return S_OK;
    }

    HRESULT __stdcall EndOperation(HRESULT /*hResult*/, IBindCtx* /*pbcReserved*/,
                                   DWORD /*dwEffects*/) noexcept override {
        in_operation_ = FALSE;
        return S_OK;
    }

    void InitData(PCIDLIST_ABSOLUTE pidlFolder, uint32_t cidl, PCUITEMID_CHILD_ARRAY ppidl,
                  const std::wstring& rootPath) {
        Init(pidlFolder, cidl, ppidl);

        std::vector<std::wstring> item_ids;
        item_ids.reserve(cidl);
        for (uint32_t index = 0; index < cidl; ++index) {
            item_ids.push_back(HostItem(ppidl[index]).GetItemId());
        }
        if (!item_ids.empty()) {
            copy_state_ = std::make_shared<ArchiveCopyState>(rootPath, std::move(item_ids));
            RegisterClipboardFormatHandler(
                std::make_unique<ArchiveFileDescriptorHandler>(copy_state_, this));
            RegisterClipboardFormatHandler(
                std::make_unique<ArchiveFileContentsHandler>(copy_state_));
        }
    }

  private:
    std::shared_ptr<ArchiveCopyState> copy_state_;
    BOOL do_async_{TRUE};
    BOOL in_operation_{FALSE};
};

} // namespace aegra::shell
