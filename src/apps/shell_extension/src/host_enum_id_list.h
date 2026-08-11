#pragma once

#include "archive_shell_model.h"
#include "host_item.h"

#include <msf.h>

#include <string>
#include <vector>

namespace aegra::shell {

// IEnumIDList over ArchiveShellModel — same filter rules as msf_host HostEnumIDList.
class __declspec(novtable) HostEnumIDList : public ATL::CComObjectRootEx<ATL::CComSingleThreadModel>,
                                            public msf::IEnumIDListImpl<HostEnumIDList> {
  public:
    DECLARE_NOT_AGGREGATABLE(HostEnumIDList)

    BEGIN_COM_MAP(HostEnumIDList)
        COM_INTERFACE_ENTRY(IEnumIDList)
    END_COM_MAP()

    [[nodiscard]] static ATL::CComPtr<IEnumIDList>
    CreateInstance(const std::wstring& rootPath, const std::wstring& folderId, DWORD grfFlags) {
        ATL::CComObject<HostEnumIDList>* instance = nullptr;
        const HRESULT hr = ATL::CComObject<HostEnumIDList>::CreateInstance(&instance);
        if (FAILED(hr)) {
            msf::RaiseException(hr);
        }
        ATL::CComPtr<IEnumIDList> enumIdList(instance);
        msf::RaiseExceptionIfFailed(instance->Initialize(rootPath, folderId, grfFlags));
        return enumIdList;
    }

    [[nodiscard]] LPITEMIDLIST GetNextItem() {
        while (m_currentIndex < m_items.size()) {
            const ItemEntry& entry = m_items[m_currentIndex];
            ++m_currentIndex;

            if (msf::IsBitSet(entry.attributes, SFGAO_HIDDEN) &&
                !msf::IsBitSet(m_grfFlags, SHCONTF_INCLUDEHIDDEN)) {
                continue;
            }
            if (entry.isFolder && !msf::IsBitSet(m_grfFlags, SHCONTF_FOLDERS) &&
                msf::IsBitSet(m_grfFlags, SHCONTF_NONFOLDERS)) {
                continue;
            }
            if (!entry.isFolder && !msf::IsBitSet(m_grfFlags, SHCONTF_NONFOLDERS) &&
                msf::IsBitSet(m_grfFlags, SHCONTF_FOLDERS)) {
                continue;
            }

            return static_cast<LPITEMIDLIST>(HostItem::CreateItemIdList(
                entry.itemId.c_str(), entry.isFolder, entry.displayName.c_str(), entry.size,
                entry.modifiedTime, entry.fileAttributes));
        }
        return nullptr;
    }

  protected:
    HostEnumIDList() noexcept = default;

  private:
    struct ItemEntry {
        std::wstring itemId;
        std::wstring displayName;
        uint64_t size{0};
        ULONGLONG modifiedTime{0};
        uint32_t attributes{0};
        uint32_t fileAttributes{FILE_ATTRIBUTE_NORMAL};
        bool isFolder{false};
    };

    [[nodiscard]] HRESULT Initialize(const std::wstring& rootPath, const std::wstring& folderId,
                                     const DWORD grfFlags) {
        m_grfFlags = grfFlags;
        m_currentIndex = 0;
        std::vector<ArchiveShellItem> rawItems;
        const HRESULT hr = ArchiveShellModel::Instance().EnumChildren(rootPath, folderId, rawItems);
        if (FAILED(hr)) {
            ATLTRACE(L"HostEnumIDList::Initialize failed hr=0x%08X\n", hr);
            return hr;
        }
        if (!rawItems.empty()) {
            m_items.reserve(rawItems.size());
            for (const auto& rawItem : rawItems) {
                ItemEntry entry;
                entry.itemId = rawItem.itemId;
                entry.displayName = rawItem.displayName;
                entry.size = rawItem.size;
                entry.modifiedTime = rawItem.modifiedTime;
                entry.attributes = rawItem.attributes;
                entry.fileAttributes = rawItem.fileAttributes;
                entry.isFolder = rawItem.isFolder;
                m_items.push_back(std::move(entry));
            }
        }
        return S_OK;
    }

    DWORD m_grfFlags{0};
    size_t m_currentIndex{0};
    std::vector<ItemEntry> m_items;
};

} // namespace aegra::shell
