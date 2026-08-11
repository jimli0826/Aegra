//
// (C) Copyright by Victor Derks
//
// See README.TXT for the details of the software license.
//
#pragma once

#include <Windows.h>

// Purpose: Stable C ABI for secondary developers to implement a virtual file system
//          provider. The MSF Host DLL (msf_host.dll) owns all COM/Shell integration
//          and forwards requests to these callbacks. Secondary developers do NOT need
//          to understand COM, ATL, IShellFolder, PIDLs, or any Shell internals.
//
// ABI Version History:
//   1 - Initial version with EnumChildren, GetItemInfo, OpenItem, RenameItem,
//       DeleteItems, GetInfoTip, FreeMemory.
//   2 - Adds OpenRoot so a provider can bind callbacks to the file/path that
//       Explorer is currently opening.

#define MSF_PROVIDER_VERSION 2

// Commonly-used SFGAO (Shell File Get Attributes Of) flags.
// Providers can use these in MsfItemInfo::attributes without including shlobj.h.
// Full list: https://docs.microsoft.com/en-us/windows/win32/shell/sfgao
#ifndef SFGAO_FOLDER
#define MSF_SFGAO_FOLDER         0x20000000  // Item is a folder
#define MSF_SFGAO_HASSUBFOLDER   0x80000000  // Folder has subfolders
#define MSF_SFGAO_BROWSABLE      0x08000000  // Item is browsable (folder that can be opened)
#define MSF_SFGAO_CANCOPY        0x00000001  // Item can be copied
#define MSF_SFGAO_CANMOVE        0x00000002  // Item can be moved
#define MSF_SFGAO_CANLINK        0x00000004  // Shortcut can be created for item
#define MSF_SFGAO_CANRENAME      0x00000010  // Item can be renamed
#define MSF_SFGAO_CANDELETE      0x00000020  // Item can be deleted
#define MSF_SFGAO_HASPROPSHEET   0x00000040  // Item has property sheet
#define MSF_SFGAO_DROPTARGET     0x00000100  // Item is a drop target
#define MSF_SFGAO_READONLY       0x00040000  // Item is read-only
#define MSF_SFGAO_HIDDEN         0x00080000  // Item is hidden
#define MSF_SFGAO_STREAM         0x00400000  // Item has a stream (file contents)
#define MSF_SFGAO_LINK           0x00010000  // Item is a shortcut/link
#else
// If shlobj.h was already included, map our names to the Windows SDK constants
#define MSF_SFGAO_FOLDER         SFGAO_FOLDER
#define MSF_SFGAO_HASSUBFOLDER   SFGAO_HASSUBFOLDER
#define MSF_SFGAO_BROWSABLE      SFGAO_BROWSABLE
#define MSF_SFGAO_CANCOPY        SFGAO_CANCOPY
#define MSF_SFGAO_CANMOVE        SFGAO_CANMOVE
#define MSF_SFGAO_CANLINK        SFGAO_CANLINK
#define MSF_SFGAO_CANRENAME      SFGAO_CANRENAME
#define MSF_SFGAO_CANDELETE      SFGAO_CANDELETE
#define MSF_SFGAO_HASPROPSHEET   SFGAO_HASPROPSHEET
#define MSF_SFGAO_DROPTARGET     SFGAO_DROPTARGET
#define MSF_SFGAO_READONLY       SFGAO_READONLY
#define MSF_SFGAO_HIDDEN         SFGAO_HIDDEN
#define MSF_SFGAO_STREAM         SFGAO_STREAM
#define MSF_SFGAO_LINK           SFGAO_LINK
#endif

// Purpose: Describes one item in a virtual folder (file or subfolder).
//          Returned by EnumChildren and GetItemInfo.
//          All string pointers are owned by the provider and freed via FreeMemory.
struct MsfItemInfo
{
    const wchar_t* id;        // stable, unique identifier within the provider (e.g. "file1", "/docs/report.txt")
    const wchar_t* name;      // display name shown in Explorer (e.g. "report.txt")
    uint64_t       size;      // file size in bytes (0 for folders)
    uint32_t       attributes;// SFGAO_* shell attributes (e.g. SFGAO_CANCOPY | SFGAO_CANRENAME)
    bool           isFolder;  // true = folder (can be navigated into), false = file
};

// Purpose: Table of callback functions implemented by the provider DLL.
//          Filled in by MsfCreateProvider. The host calls these to interact
//          with the virtual file system.
//
// ABI stability: `size` and `version` fields allow the host to detect
// version mismatches. New callbacks may be appended in future versions;
// the version field tells the host which fields are valid.
struct MsfCallbacks
{
    uint32_t size;     // sizeof(MsfCallbacks) - set by host before calling MsfCreateProvider
    uint32_t version;  // MSF_PROVIDER_VERSION - set by provider to indicate which ABI it uses
    void*    userContext; // opaque pointer passed to all callbacks (provider-private data)

    // Mandatory: Bind the provider to the root file/path that Explorer is opening.
    // - rootPath: the file or namespace path represented by the current ShellFolder instance
    // Returns S_OK on success, error HRESULT on failure.
    HRESULT (*OpenRoot)(void* userContext, const wchar_t* rootPath);

    // Mandatory: Enumerate all children of a folder.
    // - folderId: identifies the folder; nullptr or empty string = root
    // - items: output — provider-allocated array of MsfItemInfo
    // - itemCount: output — number of items in the array
    // Returns S_OK on success, error HRESULT on failure.
    // Memory is freed by the host calling FreeMemory(items).
    HRESULT (*EnumChildren)(void* userContext, const wchar_t* folderId,
                            MsfItemInfo** items, uint32_t* itemCount);

    // Mandatory: Get information about a single item by its ID.
    // - itemId: the item's unique identifier (as returned in MsfItemInfo.id)
    // - itemInfo: output — filled with the item's current info
    // Returns S_OK on success, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) if item unknown.
    HRESULT (*GetItemInfo)(void* userContext, const wchar_t* itemId,
                           MsfItemInfo* itemInfo);

    // Optional (may be null): Called when the user double-clicks or presses Enter on a file.
    // - hwnd: parent window handle for UI
    // - itemId: the file to open
    // Folders are handled by the host (navigating into them) — OpenItem is only called for files.
    // Returns S_OK on success, error HRESULT on failure.
    HRESULT (*OpenItem)(void* userContext, HWND hwnd, const wchar_t* itemId);

    // Optional (may be null): Called when the user renames an item.
    // - itemId: the item to rename
    // - newName: the new display name
    // - newInfo: output — filled with the updated item info (may reuse same id, change name)
    // Returns S_OK on success, error HRESULT on failure.
    HRESULT (*RenameItem)(void* userContext, const wchar_t* itemId,
                          const wchar_t* newName, MsfItemInfo* newInfo);

    // Optional (may be null): Called when the user deletes one or more items.
    // - itemIds: array of item identifiers to delete
    // - itemCount: number of items in the array
    // Returns S_OK on success, error HRESULT on failure.
    HRESULT (*DeleteItems)(void* userContext, const wchar_t** itemIds, uint32_t itemCount);

    // Optional (may be null): Called when Explorer wants an info tip (tooltip) for an item.
    // - itemId: the item to get info for
    // - text: output — provider-allocated string (freed via FreeMemory)
    // Returns S_OK on success; return S_FALSE or E_NOTIMPL to show no tip.
    HRESULT (*GetInfoTip)(void* userContext, const wchar_t* itemId, wchar_t** text);

    // Mandatory: Free memory allocated by the provider.
    // Called by the host for every pointer the provider returned (item arrays, strings, etc.).
    // - memory: pointer to free (may be null — should be a no-op in that case)
    void (*FreeMemory)(void* userContext, void* memory);
};

// Purpose: The ONE and ONLY export a provider DLL must implement.
//          The host calls this after loading the DLL. The provider fills in
//          the MsfCallbacks structure with its implementation.
//
// Example:
//   extern "C" __declspec(dllexport)
//   HRESULT MsfCreateProvider(MsfCallbacks* callbacks)
//   {
//       if (callbacks->size < sizeof(MsfCallbacks))
//           return E_INVALIDARG;
//       callbacks->version = MSF_PROVIDER_VERSION;
//       callbacks->OpenRoot     = MyOpenRoot;
//       callbacks->EnumChildren = MyEnumChildren;
//       callbacks->GetItemInfo  = MyGetItemInfo;
//       callbacks->OpenItem     = MyOpenItem;
//       callbacks->RenameItem   = MyRenameItem;
//       callbacks->DeleteItems  = MyDeleteItems;
//       callbacks->GetInfoTip   = MyGetInfoTip;
//       callbacks->FreeMemory   = MyFreeMemory;
//       callbacks->userContext  = &g_myData;
//       return S_OK;
//   }
//
extern "C" HRESULT MsfCreateProvider(MsfCallbacks* callbacks);
