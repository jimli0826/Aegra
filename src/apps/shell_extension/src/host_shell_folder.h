#pragma once

#include "archive_shell_data_object.h"
#include "archive_shell_model.h"
#include "host_enum_id_list.h"
#include "host_item.h"
#include "host_shell_folder_class_id.h"
#include "resource.h"
#include "shell_strings.h"

#include <msf.h>

#include <string>
#include <vector>

namespace aegra::shell {

// MSF ShellFolderImpl host — COM surface only; browsing is ArchiveShellModel.
class __declspec(novtable) HostShellFolder
    : public ATL::CComObjectRootEx<ATL::CComSingleThreadModel>,
      public ATL::CComCoClass<HostShellFolder, &__uuidof(HostShellFolder)>,
      public msf::ShellFolderImpl<HostShellFolder, HostItem>,
      public msf::IBrowserFrameOptionsImpl,
      public msf::IItemNameLimitsImpl<HostShellFolder, HostItem> {
  public:
    BEGIN_COM_MAP(HostShellFolder)
        COM_INTERFACE_ENTRY2(IPersist, IPersistFolder)
        COM_INTERFACE_ENTRY(IPersistFolder)
        COM_INTERFACE_ENTRY(IPersistFolder2)
        COM_INTERFACE_ENTRY(IPersistFolder3)
        COM_INTERFACE_ENTRY(IPersistIDList)
        COM_INTERFACE_ENTRY(IShellFolder)
        COM_INTERFACE_ENTRY(IShellFolder2)
        COM_INTERFACE_ENTRY(IShellDetails)
        COM_INTERFACE_ENTRY(IBrowserFrameOptions)
        COM_INTERFACE_ENTRY(IShellIcon)
        COM_INTERFACE_ENTRY(IItemNameLimits)
        COM_INTERFACE_ENTRY(IDropTarget)
        COM_INTERFACE_ENTRY(IObjectWithFolderEnumMode)
        COM_INTERFACE_ENTRY(IExplorerPaneVisibility)
    END_COM_MAP()

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    static HRESULT __stdcall UpdateRegistry(BOOL bRegister) noexcept {
        return msf::ShellFolderImpl<HostShellFolder, HostItem>::UpdateRegistry(
            bRegister, IDR_SHELLFOLDER, L"Aegra Backup Archive", kFileRootProgId,
            IDS_SHELLFOLDER_TYPE);
    }

    void InitializeSubFolder(const std::vector<HostItem>& items) {
        m_folderId.clear();
        if (!items.empty()) {
            m_folderId = items.back().GetItemId();
        }
    }

    [[nodiscard]] ATL::CComPtr<IShellFolderViewCB> CreateShellFolderViewCB() const {
        return nullptr;
    }

    // Match AIVImage: authenticate before SHCreateShellFolderView. Returning nullptr on Cancel
    // makes Explorer abandon this navigation and leave the current folder unchanged.
    [[nodiscard]] ATL::CComPtr<IShellView> CreateShellFolderView() {
        const HRESULT hr = ArchiveShellModel::Instance().PrepareFolderView(GetRootPath());
        if (FAILED(hr)) {
            if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
                hr != HRESULT_FROM_WIN32(ERROR_BUSY)) {
                OnError(hr, GetHwndOwner(), ErrorContext::Unknown);
            }
            return nullptr;
        }
        return msf::ShellFolderImpl<HostShellFolder, HostItem>::CreateShellFolderView();
    }

    // File descriptors + contents for Explorer copy / drag-drop (not bare PIDL data).
    [[nodiscard]] ATL::CComPtr<IDataObject>
    CreateDataObject(PCIDLIST_ABSOLUTE pidlFolder, uint32_t cidl, PCUITEMID_CHILD_ARRAY ppidl) {
        ATL::CComObject<ArchiveShellDataObject>* instance = nullptr;
        msf::RaiseExceptionIfFailed(
            ATL::CComObject<ArchiveShellDataObject>::CreateInstance(&instance));
        ATL::CComPtr<IDataObject> data_object(instance);
        instance->InitData(pidlFolder, cidl, ppidl, GetRootPath());
        return data_object;
    }

    [[nodiscard]] ATL::CComPtr<IEnumIDList> CreateEnumIDList(HWND /*hwnd*/, DWORD grfFlags) const {
        return HostEnumIDList::CreateInstance(GetRootPath(), m_folderId, grfFlags);
    }

    [[nodiscard]] SFGAOF GetAttributeOf(unsigned int cidl, const HostItem& item,
                                        SFGAOF /*mask*/) const {
        ArchiveShellItem info;
        const HRESULT hr =
            ArchiveShellModel::Instance().GetItemInfo(GetRootPath(), item.GetItemId(), info);
        if (SUCCEEDED(hr)) {
            return static_cast<SFGAOF>(info.attributes);
        }
        return item.GetAttributeOf(cidl == 1, true);
    }

    [[nodiscard]] ATL::CComPtr<IQueryInfo> CreateQueryInfo(const HostItem& item) {
        std::wstring tip;
        const HRESULT hr =
            ArchiveShellModel::Instance().GetInfoTip(GetRootPath(), item.GetItemId(), tip);
        if (FAILED(hr) || hr == S_FALSE || tip.empty()) {
            return nullptr;
        }
        return msf::QueryInfo::CreateInstance(std::move(tip));
    }

    static EXPLORERPANESTATE GetPaneState(_In_ REFEXPLORERPANE ep) noexcept {
        if (ep == EP_Ribbon || ep == EP_DetailsPane) {
            return EPS_DEFAULT_ON;
        }
        return EPS_DONTCARE;
    }

    static HRESULT OnDfmMergeContextMenu(IDataObject* dataObject, uint32_t /*uFlags*/,
                                         QCMINFO& mergeInfo) {
        const msf::CfShellIdList itemList(dataObject);
        if (itemList.size() == 1 && !HostItem(itemList.GetItem(0)).IsFolder()) {
            constexpr uint32_t kOpenCmdId = 0;
            const msf::CMenu menu(true);
            menu.AddDefaultItem(kOpenCmdId, L"&Open");
            MergeMenus(mergeInfo, menu);
        }
        return S_OK;
    }

    static std::wstring OnDfmGetHelpText(unsigned short nCmdId) {
        return load_shell_string(IDS_SHELLFOLDER_DFM_HELP_OPEN + nCmdId);
    }

    HRESULT OnDfmInvokeAddedCommand(HWND hwnd, IDataObject* dataObject, int nId) const {
        if (nId == 0) {
            OnOpen(hwnd, dataObject);
        }
        return S_OK;
    }

    void OnOpen(HWND hwnd, IDataObject* dataObject) const {
        msf::CfShellIdList list(dataObject);
        ATLASSERT(list.size() == 1);
        const HostItem item(list.GetItem(0));
        if (item.IsFolder()) {
            GetShellBrowser().BrowseObject(item.GetItemIdList(), SBSP_DEFBROWSER | SBSP_RELATIVE);
            return;
        }
        const HRESULT hr =
            ArchiveShellModel::Instance().OpenItem(GetRootPath(), hwnd, item.GetItemId());
        if (FAILED(hr)) {
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                return;
            }
            const bool too_large = hr == HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            const auto message = too_large ? load_shell_string(IDS_ERR_FILE_TOO_LARGE)
                                           : format_shell_string(IDS_OPEN_ITEM_FAILED, item.GetName());
            IsolationAwareMessageBox(hwnd, message.c_str(),
                                     load_shell_string(IDS_SHELLEXT_ERROR_CAPTION).c_str(),
                                     MB_OK | (too_large ? MB_ICONWARNING : MB_ICONERROR));
        }
    }

    PUIDLIST_RELATIVE OnSetNameOf(HWND, const HostItem&, const wchar_t*, SHGDNF) const {
        return nullptr;
    }

    long OnDelete(HWND, const std::vector<HostItem>&) const {
        return 0;
    }

    long OnProperties(HWND, std::vector<HostItem>&) {
        return 0;
    }

    static void OnError(HRESULT hr, HWND hwnd, ErrorContext) noexcept {
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            return;
        }
        try {
            auto message = load_shell_string(IDS_SHELLFOLDER_CANNOT_PERFORM) +
                           msf::FormatLastError(static_cast<DWORD>(hr));
            IsolationAwareMessageBox(hwnd, message.c_str(),
                                     load_shell_string(IDS_SHELLEXT_ERROR_CAPTION).c_str(),
                                     MB_OK | MB_ICONERROR);
        } catch (...) {
        }
    }

  protected:
    HostShellFolder() noexcept {
        RegisterColumn(load_shell_string(IDS_SHELLEXT_NAME).c_str(), LVCFMT_LEFT);
        RegisterColumn(load_shell_string(IDS_COLUMN_DATE_MODIFIED).c_str(), LVCFMT_LEFT);
        RegisterColumn(load_shell_string(IDS_COLUMN_TYPE).c_str(), LVCFMT_LEFT);
        RegisterColumn(load_shell_string(IDS_SHELLEXT_SIZE).c_str(), LVCFMT_RIGHT);
    }

  private:
    [[nodiscard]] bool IsReadOnly() const noexcept {
        return true;
    }

    [[nodiscard]] std::wstring GetRootPath() const {
        return GetPathJunctionPoint();
    }

    std::wstring m_folderId;
};

} // namespace aegra::shell
