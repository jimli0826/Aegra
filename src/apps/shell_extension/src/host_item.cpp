//
// (C) Copyright by Victor Derks
//
// See README.TXT for the details of the software license.
//

#include "pch.h"

#include "host_item.h"
#include "resource.h"
#include "shell_strings.h"

#include <cwchar>

using std::wstring;

namespace aegra::shell {

namespace
{
constexpr int kColumnName = 0;
constexpr int kColumnDateModified = 1;
constexpr int kColumnType = 2;
constexpr int kColumnSize = 3;

std::wstring FormatSize(uint64_t size)
{
    const uint64_t kb = size == 0 ? 0 : ((size + 1023) / 1024);

    wchar_t number[64]{};
    swprintf_s(number, L"%llu", static_cast<unsigned long long>(kb));

    wchar_t formatted[64]{};
    NUMBERFMTW format{};
    format.NumDigits = 0;
    format.LeadingZero = 1;
    format.Grouping = 3;
    format.lpDecimalSep = const_cast<LPWSTR>(L".");
    format.lpThousandSep = const_cast<LPWSTR>(L",");
    format.NegativeOrder = 1;
    if (::GetNumberFormatW(LOCALE_USER_DEFAULT, 0, number, &format, formatted, _countof(formatted)) > 0)
        return std::wstring(formatted) + L" KB";

    return std::wstring(number) + L" KB";
}

std::wstring FormatFileTime(ULONGLONG packedFileTime)
{
    if (packedFileTime == 0)
        return {};

    FILETIME utc{};
    utc.dwLowDateTime = static_cast<DWORD>(packedFileTime & 0xFFFFFFFFu);
    utc.dwHighDateTime = static_cast<DWORD>(packedFileTime >> 32);

    FILETIME local{};
    if (!::FileTimeToLocalFileTime(&utc, &local))
        return {};

    SYSTEMTIME st{};
    if (!::FileTimeToSystemTime(&local, &st))
        return {};

    wchar_t dateText[64]{};
    wchar_t timeText[64]{};
    if (!::GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, dateText, _countof(dateText)))
        return {};
    if (!::GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, timeText, _countof(timeText)))
        return dateText;

    return std::wstring(dateText) + L" " + timeText;
}

[[nodiscard]] bool item_id_starts_with(const wchar_t* itemId, const wchar_t* prefix) noexcept
{
    if (itemId == nullptr || prefix == nullptr)
        return false;
    const size_t n = wcslen(prefix);
    return wcsncmp(itemId, prefix, n) == 0;
}

[[nodiscard]] int get_stock_sys_icon(SHSTOCKICONID stockId) noexcept
{
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    if (SUCCEEDED(::SHGetStockIconInfo(stockId, SHGSI_SYSICONINDEX, &info)))
        return info.iSysImageIndex;

    // Fallback: real drive root if stock API fails.
    SHFILEINFOW fileInfo{};
    if (::SHGetFileInfoW(L"C:\\", 0, &fileInfo, sizeof(fileInfo), SHGFI_SYSICONINDEX) != 0)
        return fileInfo.iIcon;

    return static_cast<int>(msf::StandardImagelistIndex::FolderPlain);
}

[[nodiscard]] std::wstring shell_type_name_from_attrs(const std::wstring& name, DWORD attrs,
                                                      unsigned fallback_id)
{
    SHFILEINFOW info{};
    if (::SHGetFileInfoW(name.c_str(), attrs, &info, sizeof(info),
                         SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) != 0 &&
        info.szTypeName[0] != L'\0') {
        return info.szTypeName;
    }
    return load_shell_string(fallback_id);
}

std::wstring GetShellTypeName(const std::wstring& name, bool isFolder, uint32_t fileAttributes,
                              const wchar_t* itemId)
{
    // Disk / Volume labels follow Shell UI language (not file association type names).
    if (item_id_starts_with(itemId, L"disk:")) {
        return load_shell_string(IDS_TYPE_DISK);
    }
    if (item_id_starts_with(itemId, L"vol:")) {
        return load_shell_string(IDS_TYPE_VOLUME);
    }

    // Prefer the system type name so "File folder" / extension types match Explorer locale.
    if (isFolder) {
        const DWORD attrs =
            (fileAttributes == 0 ? FILE_ATTRIBUTE_DIRECTORY
                                 : (fileAttributes | FILE_ATTRIBUTE_DIRECTORY));
        return shell_type_name_from_attrs(name, attrs, IDS_TYPE_FILE_FOLDER);
    }

    const DWORD attrs = fileAttributes == 0 ? FILE_ATTRIBUTE_NORMAL : fileAttributes;
    return shell_type_name_from_attrs(name, attrs, IDS_TYPE_FILE);
}

int GetShellIconIndex(const std::wstring& name, bool isFolder, uint32_t flags,
                      uint32_t fileAttributes, const wchar_t* itemId) noexcept
{
    // Disk / volume nodes: system fixed-drive icon (not a plain folder).
    if (item_id_starts_with(itemId, L"disk:") || item_id_starts_with(itemId, L"vol:"))
        return get_stock_sys_icon(SIID_DRIVEFIXED);

    SHFILEINFOW info{};
    const DWORD attrs = isFolder ? FILE_ATTRIBUTE_DIRECTORY :
        (fileAttributes == 0 ? FILE_ATTRIBUTE_NORMAL : fileAttributes);
    UINT shFlags = SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES;
    if (isFolder && msf::IsBitSet(flags, GIL_OPENICON))
        shFlags |= SHGFI_OPENICON;

    if (::SHGetFileInfoW(name.c_str(), attrs, &info, sizeof(info), shFlags) != 0)
        return info.iIcon;

    return isFolder
        ? static_cast<int>(msf::StandardImagelistIndex::FolderPlain)
        : static_cast<int>(msf::StandardImagelistIndex::DocumentFilled);
}

/// Scan for a NUL-terminated wide string that starts at @p start and ends before @p end.
/// On success returns the character count excluding the terminator; on failure npos.
[[nodiscard]] size_t bounded_wcs_len(const wchar_t* start, const wchar_t* end) noexcept
{
    if (start == nullptr || end == nullptr || start >= end)
        return static_cast<size_t>(-1);
    const wchar_t* p = start;
    while (p < end) {
        if (*p == L'\0')
            return static_cast<size_t>(p - start);
        ++p;
    }
    return static_cast<size_t>(-1);
}
} // namespace

bool HostItem::TryParseItemStrings(const SItemData* data, const size_t data_size,
                                   const wchar_t** name_out, const wchar_t** item_id_out) noexcept
{
    if (name_out == nullptr || item_id_out == nullptr)
        return false;
    *name_out = nullptr;
    *item_id_out = nullptr;

    constexpr size_t kNameOffset = offsetof(SItemData, cachedName);
    // Need room for at least: empty name + NUL + empty itemId + NUL.
    if (data == nullptr || data_size < kNameOffset + 2 * sizeof(wchar_t))
        return false;
    if (data->typeId != kTypeID)
        return false;

    const auto* const base = reinterpret_cast<const unsigned char*>(data);
    // SItemData is pack(1); cachedName may be unaligned — still safe to read as wchar_t on x86/x64.
    const auto* const name = reinterpret_cast<const wchar_t*>(base + kNameOffset);
    const size_t max_chars = (data_size - kNameOffset) / sizeof(wchar_t);
    const wchar_t* const chars_end = name + max_chars;

    const size_t name_len = bounded_wcs_len(name, chars_end);
    if (name_len == static_cast<size_t>(-1))
        return false;

    const wchar_t* const item_id = name + name_len + 1;
    const size_t id_len = bounded_wcs_len(item_id, chars_end);
    if (id_len == static_cast<size_t>(-1))
        return false;

    // Both strings (including terminators) must fit in the declared PIDL payload.
    const size_t used_bytes = kNameOffset + (name_len + 1 + id_len + 1) * sizeof(wchar_t);
    if (used_bytes > data_size)
        return false;

    *name_out = name;
    *item_id_out = item_id;
    return true;
}

PUIDLIST_RELATIVE HostItem::CreateItemIdList(
    const wchar_t* itemId,
    bool isFolder,
    const wchar_t* displayName,
    uint64_t size,
    ULONGLONG modifiedTime,
    uint32_t fileAttributes)
{
    const size_t nameLen = wcslen(displayName);
    const size_t idLen = wcslen(itemId);

    const size_t dataSize = offsetof(SItemData, cachedName)
        + (nameLen + 1) * sizeof(wchar_t)
        + (idLen + 1) * sizeof(wchar_t);

    const auto pidl = msf::ItemIDList::CreateItemIdListWithTerminator(dataSize);

    auto* pData = reinterpret_cast<SItemData*>(pidl->mkid.abID);
    pData->typeId = kTypeID;
    pData->isFolder = isFolder;
    pData->size = size;
    pData->modifiedTime = modifiedTime;
    pData->fileAttributes = fileAttributes;

    wchar_t* pName = pData->cachedName;
    wcscpy_s(pName, nameLen + 1, displayName);

    wchar_t* pItemId = pName + nameLen + 1;
    wcscpy_s(pItemId, idLen + 1, itemId);

    return pidl;
}

HostItem::HostItem(PCUIDLIST_RELATIVE pidl)
    : msf::ItemBase(pidl)
{
    const wchar_t* name = nullptr;
    const wchar_t* item_id = nullptr;
    const bool valid =
        TryParseItemStrings(GetItemDataPtr(), GetDataSize(), &name, &item_id);

#ifdef _DEBUG
    if (!valid)
    {
        const auto* data = GetItemDataPtr();
        ATLTRACE(L"HostItem::Constructor: PIDL not valid (data_size=%u, typeId=0x%04X, expected=0x%04X)\n",
                 static_cast<unsigned>(GetDataSize()),
                 data != nullptr ? data->typeId : 0,
                 kTypeID);
    }
#endif
    msf::RaiseExceptionIf(!valid);

    m_isFolder = GetItemDataPtr()->isFolder;
    m_itemId = item_id;
    (void)name; // GetName() re-reads cachedName; bounds already validated above.
}

std::wstring HostItem::GetDisplayName(SHGDNF shellGetDisplayNameType) const
{
    switch (shellGetDisplayNameType)
    {
    case SHGDN_NORMAL:
    case SHGDN_INFOLDER:
    case SHGDN_INFOLDER | SHGDN_FOREDITING:
    case SHGDN_INFOLDER | SHGDN_FORADDRESSBAR:
        break;

    case SHGDN_FORPARSING:
    case SHGDN_INFOLDER | SHGDN_FORPARSING:
        return m_itemId;

    default:
        ATLTRACE(L"HostItem::GetDisplayName (shellGetDisplayNameType=%d)\n", shellGetDisplayNameType);
        break;
    }

    return GetName();
}

SFGAOF HostItem::GetAttributeOf(bool bSingleSelect, bool bReadOnly) const noexcept
{
    if (IsFolder()) {
        return SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE | SFGAO_CANCOPY |
               SFGAO_READONLY;
    }

    SFGAOF flags = SFGAO_CANCOPY | SFGAO_STREAM | SFGAO_READONLY;
    if (!bReadOnly)
        flags |= SFGAO_CANRENAME | SFGAO_CANDELETE | SFGAO_CANMOVE;
    if (bSingleSelect)
        flags |= SFGAO_HASPROPSHEET;
    return flags;
}

int HostItem::Compare(const HostItem& item, int compareBy, bool /*bCanonicalOnly*/) const noexcept
{
    switch (compareBy)
    {
    case kColumnName:
        return CompareByName(*this, item);

    case kColumnDateModified:
        return GetModifiedTime() < item.GetModifiedTime() ? -1 :
            (GetModifiedTime() > item.GetModifiedTime() ? 1 : 0);

    case kColumnType:
        return StrCmpIW(
            GetShellTypeName(GetName(), IsFolder(), GetFileAttributes(), GetItemId()).c_str(),
            GetShellTypeName(item.GetName(), item.IsFolder(), item.GetFileAttributes(),
                             item.GetItemId())
                .c_str());

    case kColumnSize:
        return GetSize() < item.GetSize() ? -1 : (GetSize() > item.GetSize() ? 1 : 0);

    default:
        ATLASSERT(!"Illegal compare option detected");
        return 1;
    }
}

std::wstring HostItem::GetItemDetailsOf(uint32_t columnIndex) const
{
    switch (columnIndex)
    {
    case kColumnName:
        return GetDisplayName(SHGDN_NORMAL);

    case kColumnDateModified:
        return FormatFileTime(GetModifiedTime());

    case kColumnType:
        return GetShellTypeName(GetName(), IsFolder(), GetFileAttributes(), GetItemId());

    case kColumnSize:
        if (IsFolder())
            return {};
        return FormatSize(GetSize());

    default:
        ATLASSERT(false);
        msf::RaiseException();
    }
}

std::wstring HostItem::GetInfoTipText() const
{
    return {};
}

int HostItem::GetIconOf(uint32_t flags) const noexcept
{
    return GetShellIconIndex(GetName(), IsFolder(), flags, GetFileAttributes(), GetItemId());
}

int HostItem::CompareByName(const HostItem& item1, const HostItem& item2) noexcept
{
    if (item1.IsFolder())
    {
        if (!item2.IsFolder())
            return -1;
    }
    else if (item2.IsFolder())
    {
        return 1;
    }

    const int result = StrCmpIW(item1.GetItemDataPtr()->cachedName,
                                item2.GetItemDataPtr()->cachedName);
    if (result != 0)
        return result;

    return StrCmpIW(item1.m_itemId, item2.m_itemId);
}

} // namespace aegra::shell

