//
// (C) Copyright by Victor Derks
//
// See README.TXT for the details of the software license.
//
#pragma once

#include <msf.h>
#include <string>

namespace aegra::shell {

// Purpose: Generic item class for the MSF Host Shell Folder.
// Replaces the sample-specific VVVItem with a provider-driven model.
//
// PIDL binary format (packed):
//   [USHORT cb] [fixed metadata] [WCHAR cachedName...] [WCHAR itemId...\0]
//
// - typeId (0x4D01 = 'M' + version 1): validates PIDLs are ours
// - isFolder: cached folder flag (1 = folder, 0 = file)
// - cachedName: null-terminated display name (stored inline in PIDL so
//               we can show names even if the provider is temporarily unavailable)
// - itemId: null-terminated unique identifier string (provider-assigned, stable)
//
// Rules:
// - No pointers in PIDLs (only plain data).
// - itemId must be stable and unique within the provider.
// - PIDLs remain valid across Shell calls (self-contained data).
// - Constructors treat external PIDLs as untrusted: both variable strings must be
//   NUL-terminated and fully contained within GetDataSize() before any wcslen/StrCmp.
class HostItem final : public msf::ItemBase
{
public:
    static constexpr USHORT kTypeID = 0x4D01; // 'M' + version 1

    // Purpose: Create a PIDL for a provider item.
    // Returns a relative PIDL allocated with CoTaskMemAlloc.
    [[nodiscard]] static PUIDLIST_RELATIVE CreateItemIdList(
        const wchar_t* itemId,
        bool isFolder,
        const wchar_t* displayName,
        uint64_t size,
        ULONGLONG modifiedTime,
        uint32_t fileAttributes);

    // Purpose: Construct a HostItem from a PIDL. Validates that the PIDL is ours.
    explicit HostItem(PCUIDLIST_RELATIVE pidl);

    // --- Accessors (parsed from PIDL data) ---

    [[nodiscard]] const wchar_t* GetItemId() const noexcept
    {
        return m_itemId;
    }

    [[nodiscard]] bool IsFolder() const noexcept
    {
        return m_isFolder;
    }

    [[nodiscard]] std::wstring GetDisplayName(SHGDNF shellGetDisplayNameType = SHGDN_NORMAL) const;

    [[nodiscard]] SFGAOF GetAttributeOf(bool bSingleSelect, bool bReadOnly) const noexcept;

    [[nodiscard]] std::wstring GetName() const
    {
        return GetItemDataPtr()->cachedName;
    }

    [[nodiscard]] uint64_t GetSize() const noexcept
    {
        return GetItemDataPtr()->size;
    }

    [[nodiscard]] ULONGLONG GetModifiedTime() const noexcept
    {
        return GetItemDataPtr()->modifiedTime;
    }

    [[nodiscard]] uint32_t GetFileAttributes() const noexcept
    {
        return GetItemDataPtr()->fileAttributes;
    }

    [[nodiscard]] int Compare(const HostItem& item, int compareBy, bool bCanonicalOnly) const noexcept;

    [[nodiscard]] std::wstring GetItemDetailsOf(uint32_t columnIndex) const;

    [[nodiscard]] std::wstring GetInfoTipText() const;

    [[nodiscard]] int GetIconOf(uint32_t flags) const noexcept;

    // Purpose: The maximum name length for rename operations.
    static int GetMaxNameLength(PCWSTR /*pszName*/) noexcept
    {
        return 255;
    }

private:
    // PIDL data layout after the cb prefix:
    #pragma pack(1)
    struct SItemData
    {
        USHORT typeId;       // kTypeID — validates PIDL origin
        bool   isFolder;     // cached folder flag
        uint64_t size;        // byte size for files
        ULONGLONG modifiedTime; // FILETIME packed as ULONGLONG
        uint32_t fileAttributes; // Win32 FILE_ATTRIBUTE_* flags
        wchar_t cachedName[1]; // variable-length: null-terminated display name, immediately followed by:
        // wchar_t itemId[];  // variable-length: null-terminated unique identifier
    };
    #pragma pack()

    [[nodiscard]] const SItemData* GetItemDataPtr() const noexcept
    {
        return static_cast<const SItemData*>(GetData());
    }

    /// Locate NUL-terminated cachedName and itemId inside @p data_size bytes of @p data.
    /// Returns false if typeId mismatches or either string is missing / not terminated in-bounds.
    [[nodiscard]] static bool TryParseItemStrings(const SItemData* data, size_t data_size,
                                                  const wchar_t** name_out,
                                                  const wchar_t** item_id_out) noexcept;

    [[nodiscard]] static int CompareByName(const HostItem& item1, const HostItem& item2) noexcept;

    // Cached pointers into PIDL data (not owning); set only after TryParseItemStrings succeeds.
    const wchar_t* m_itemId = nullptr;   // points into PIDL after cachedName's null terminator
    bool m_isFolder = false;
};

} // namespace aegra::shell

