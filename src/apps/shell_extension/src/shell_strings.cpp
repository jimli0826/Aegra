#include "pch.h"

#include "shell_strings.h"

#include "resource.h"

#include <array>

namespace aegra::shell {
namespace {

// Languages match backup ShellExtension DetectShellUiLang.
enum class ShellUiLang : std::uint8_t {
    En = 0,
    ZhCn = 1,
    ZhTw = 2,
    Ja = 3,
    De = 4,
};

[[nodiscard]] ShellUiLang detect_shell_ui_lang() noexcept {
    const LANGID lang = ::GetUserDefaultUILanguage();
    const WORD primary = PRIMARYLANGID(lang);
    const WORD sub = SUBLANGID(lang);
    if (primary == LANG_CHINESE) {
        if (sub == SUBLANG_CHINESE_TRADITIONAL || sub == SUBLANG_CHINESE_HONGKONG ||
            sub == SUBLANG_CHINESE_MACAU) {
            return ShellUiLang::ZhTw;
        }
        return ShellUiLang::ZhCn;
    }
    if (primary == LANG_JAPANESE) {
        return ShellUiLang::Ja;
    }
    if (primary == LANG_GERMAN) {
        return ShellUiLang::De;
    }
    return ShellUiLang::En;
}

// Unicode escapes keep the translation unit ASCII-safe under MSVC default code pages
// (same approach as backup ShellExtension multi-language strings).
struct ShellStringEntry final {
    unsigned id;
    const wchar_t* en;
    const wchar_t* zh_cn;
    const wchar_t* zh_tw;
    const wchar_t* ja;
    const wchar_t* de;
};

// clang-format off
constexpr std::array kShellStrings{
    ShellStringEntry{IDS_SHELLEXT_NAME, L"Name",
                     L"\u540d\u79f0", L"\u540d\u7a31", L"\u540d\u524d", L"Name"},
    ShellStringEntry{IDS_SHELLEXT_SIZE, L"Size",
                     L"\u5927\u5c0f", L"\u5927\u5c0f", L"\u30b5\u30a4\u30ba", L"Gr\u00f6\u00dfe"},
    ShellStringEntry{IDS_SHELLEXT_ERROR_CAPTION, L"Aegra",
                     L"Aegra", L"Aegra", L"Aegra", L"Aegra"},
    ShellStringEntry{IDS_SHELLFOLDER_DELETE,
                     L"Are you sure that you want to delete '%1'?",
                     L"\u786e\u5b9a\u8981\u5220\u9664\u201c%1\u201d\u5417\uff1f",
                     L"\u78ba\u5b9a\u8981\u522a\u9664\u300c%1\u300d\u55ce\uff1f",
                     L"\u300c%1\u300d\u3092\u524a\u9664\u3057\u3066\u3082\u3088\u308d\u3057\u3044\u3067\u3059\u304b?",
                     L"M\u00f6chten Sie \u201e%1\u201c wirklich l\u00f6schen?"},
    ShellStringEntry{IDS_SHELLFOLDER_MULTIPLE_DELETE,
                     L"Are you sure that you want to delete these %1 items?",
                     L"\u786e\u5b9a\u8981\u5220\u9664\u8fd9 %1 \u4e2a\u9879\u5417\uff1f",
                     L"\u78ba\u5b9a\u8981\u522a\u9664\u9019 %1 \u500b\u9805\u76ee\u55ce\uff1f",
                     L"\u3053\u308c\u3089\u306e %1 \u500b\u306e\u9805\u76ee\u3092\u524a\u9664\u3057\u3066\u3082\u3088\u308d\u3057\u3044\u3067\u3059\u304b?",
                     L"M\u00f6chten Sie diese %1 Elemente wirklich l\u00f6schen?"},
    ShellStringEntry{IDS_SHELLFOLDER_FILE_DELETE_CAPTION, L"Confirm File Delete",
                     L"\u786e\u8ba4\u5220\u9664\u6587\u4ef6", L"\u78ba\u8a8d\u522a\u9664\u6a94\u6848",
                     L"\u30d5\u30a1\u30a4\u30eb\u524a\u9664\u306e\u78ba\u8a8d", L"Dateil\u00f6schung best\u00e4tigen"},
    ShellStringEntry{IDS_SHELLFOLDER_FILES_DELETE_CAPTION, L"Confirm Multiple File Delete",
                     L"\u786e\u8ba4\u5220\u9664\u591a\u4e2a\u6587\u4ef6", L"\u78ba\u8a8d\u522a\u9664\u591a\u500b\u6a94\u6848",
                     L"\u8907\u6570\u30d5\u30a1\u30a4\u30eb\u524a\u9664\u306e\u78ba\u8a8d",
                     L"L\u00f6schen mehrerer Dateien best\u00e4tigen"},
    ShellStringEntry{IDS_SHELLFOLDER_TYPE, L"Aegra Backup Archive",
                     L"Aegra \u5907\u4efd\u5f52\u6863", L"Aegra \u5099\u4efd\u5c01\u5b58\u6a94",
                     L"Aegra \u30d0\u30c3\u30af\u30a2\u30c3\u30d7 \u30a2\u30fc\u30ab\u30a4\u30d6",
                     L"Aegra-Sicherungsarchiv"},
    ShellStringEntry{IDS_SHELLFOLDER_CANNOT_PERFORM,
                     L"Unable to perform the requested operation, reason:\n\n",
                     L"\u65e0\u6cd5\u6267\u884c\u8bf7\u6c42\u7684\u64cd\u4f5c\uff0c\u539f\u56e0\uff1a\n\n",
                     L"\u7121\u6cd5\u57f7\u884c\u8acb\u6c42\u7684\u64cd\u4f5c\uff0c\u539f\u56e0\uff1a\n\n",
                     L"\u8981\u6c42\u3055\u308c\u305f\u64cd\u4f5c\u3092\u5b9f\u884c\u3067\u304d\u307e\u305b\u3093\u3002\u7406\u7531:\n\n",
                     L"Die angeforderte Operation kann nicht ausgef\u00fchrt werden. Grund:\n\n"},
    ShellStringEntry{IDS_SHELLFOLDER_DFM_HELP_OPEN, L"Open",
                     L"\u6253\u5f00", L"\u958b\u555f", L"\u958b\u304f", L"\u00d6ffnen"},

    ShellStringEntry{IDS_PASSWORD_CAPTION, L"Password required \u2014 %1",
                     L"\u9700\u8981\u5bc6\u7801 \u2014 %1", L"\u9700\u8981\u5bc6\u78bc \u2014 %1",
                     L"\u30d1\u30b9\u30ef\u30fc\u30c9\u304c\u5fc5\u8981\u3067\u3059 \u2014 %1",
                     L"Kennwort erforderlich \u2014 %1"},
    ShellStringEntry{IDS_PASSWORD_PROMPT,
                     L"This archive is encrypted. Enter the password to browse it.",
                     L"\u6b64\u5f52\u6863\u5df2\u52a0\u5bc6\u3002\u8bf7\u8f93\u5165\u5bc6\u7801\u4ee5\u6d4f\u89c8\u3002",
                     L"\u6b64\u5c01\u5b58\u6a94\u5df2\u52a0\u5bc6\u3002\u8acb\u8f38\u5165\u5bc6\u78bc\u4ee5\u700f\u89bd\u3002",
                     L"\u3053\u306e\u30a2\u30fc\u30ab\u30a4\u30d6\u306f\u6697\u53f7\u5316\u3055\u308c\u3066\u3044\u307e\u3059\u3002\u53c2\u7167\u3059\u308b\u306b\u306f\u30d1\u30b9\u30ef\u30fc\u30c9\u3092\u5165\u529b\u3057\u3066\u304f\u3060\u3055\u3044\u3002",
                     L"Dieses Archiv ist verschl\u00fcsselt. Geben Sie das Kennwort ein, um es zu durchsuchen."},
    ShellStringEntry{IDS_PASSWORD_INCORRECT, L"Password is incorrect.",
                     L"\u5bc6\u7801\u4e0d\u6b63\u786e\u3002", L"\u5bc6\u78bc\u4e0d\u6b63\u78ba\u3002",
                     L"\u30d1\u30b9\u30ef\u30fc\u30c9\u304c\u6b63\u3057\u304f\u3042\u308a\u307e\u305b\u3093\u3002",
                     L"Das Kennwort ist falsch."},
    ShellStringEntry{IDS_PASSWORD_OK, L"OK",
                     L"\u786e\u5b9a", L"\u78ba\u5b9a", L"OK", L"OK"},
    ShellStringEntry{IDS_PASSWORD_CANCEL, L"Cancel",
                     L"\u53d6\u6d88", L"\u53d6\u6d88", L"\u30ad\u30e3\u30f3\u30bb\u30eb", L"Abbrechen"},

    ShellStringEntry{IDS_ERR_ARCHIVE_ENCRYPTED,
                     L"This archive is encrypted. Enter the password to open it.",
                     L"\u6b64\u5f52\u6863\u5df2\u52a0\u5bc6\u3002\u8bf7\u8f93\u5165\u5bc6\u7801\u4ee5\u6253\u5f00\u3002",
                     L"\u6b64\u5c01\u5b58\u6a94\u5df2\u52a0\u5bc6\u3002\u8acb\u8f38\u5165\u5bc6\u78bc\u4ee5\u958b\u555f\u3002",
                     L"\u3053\u306e\u30a2\u30fc\u30ab\u30a4\u30d6\u306f\u6697\u53f7\u5316\u3055\u308c\u3066\u3044\u307e\u3059\u3002\u958b\u304f\u306b\u306f\u30d1\u30b9\u30ef\u30fc\u30c9\u3092\u5165\u529b\u3057\u3066\u304f\u3060\u3055\u3044\u3002",
                     L"Dieses Archiv ist verschl\u00fcsselt. Geben Sie das Kennwort ein, um es zu \u00f6ffnen."},
    ShellStringEntry{IDS_ERR_INVALID_PASSWORD, L"Invalid archive password.",
                     L"\u5f52\u6863\u5bc6\u7801\u65e0\u6548\u3002", L"\u5c01\u5b58\u6a94\u5bc6\u78bc\u7121\u6548\u3002",
                     L"\u30a2\u30fc\u30ab\u30a4\u30d6\u306e\u30d1\u30b9\u30ef\u30fc\u30c9\u304c\u7121\u52b9\u3067\u3059\u3002",
                     L"Ung\u00fcltiges Archivkennwort."},
    ShellStringEntry{IDS_ERR_PASSWORD_CANCELLED, L"Password entry cancelled.",
                     L"\u5df2\u53d6\u6d88\u8f93\u5165\u5bc6\u7801\u3002", L"\u5df2\u53d6\u6d88\u8f38\u5165\u5bc6\u78bc\u3002",
                     L"\u30d1\u30b9\u30ef\u30fc\u30c9\u5165\u529b\u304c\u30ad\u30e3\u30f3\u30bb\u30eb\u3055\u308c\u307e\u3057\u305f\u3002",
                     L"Kennworteingabe abgebrochen."},
    ShellStringEntry{IDS_ERR_PARENT_MISSING,
                     L"Parent recovery point is missing. Open this archive from its managed repository.",
                     L"\u7f3a\u5c11\u7236\u7ea7\u6062\u590d\u70b9\u3002\u8bf7\u4ece\u5176\u7ba1\u7406\u4ed3\u5e93\u6253\u5f00\u6b64\u5f52\u6863\u3002",
                     L"\u7f3a\u5c11\u7236\u7d1a\u6062\u5fa9\u9ede\u3002\u8acb\u5f9e\u5176\u7ba1\u7406\u5132\u5b58\u5eab\u958b\u555f\u6b64\u5c01\u5b58\u6a94\u3002",
                     L"\u89aa\u306e\u5fa9\u5143\u30dd\u30a4\u30f3\u30c8\u304c\u898b\u3064\u304b\u308a\u307e\u305b\u3093\u3002\u7ba1\u7406\u30ea\u30dd\u30b8\u30c8\u30ea\u304b\u3089\u3053\u306e\u30a2\u30fc\u30ab\u30a4\u30d6\u3092\u958b\u3044\u3066\u304f\u3060\u3055\u3044\u3002",
                     L"\u00dcbergeordneter Wiederherstellungspunkt fehlt. \u00d6ffnen Sie dieses Archiv aus dem verwalteten Repository."},
    ShellStringEntry{IDS_ERR_UNSUPPORTED_VERSION, L"Unsupported archive version.",
                     L"\u4e0d\u652f\u6301\u7684\u5f52\u6863\u7248\u672c\u3002", L"\u4e0d\u652f\u63f4\u7684\u5c01\u5b58\u6a94\u7248\u672c\u3002",
                     L"\u30b5\u30dd\u30fc\u30c8\u3055\u308c\u3066\u3044\u306a\u3044\u30a2\u30fc\u30ab\u30a4\u30d6 \u30d0\u30fc\u30b8\u30e7\u30f3\u3067\u3059\u3002",
                     L"Nicht unterst\u00fctzte Archivversion."},
    ShellStringEntry{IDS_ERR_UNABLE_OPEN, L"Unable to open archive.",
                     L"\u65e0\u6cd5\u6253\u5f00\u5f52\u6863\u3002", L"\u7121\u6cd5\u958b\u555f\u5c01\u5b58\u6a94\u3002",
                     L"\u30a2\u30fc\u30ab\u30a4\u30d6\u3092\u958b\u3051\u307e\u305b\u3093\u3002",
                     L"Archiv kann nicht ge\u00f6ffnet werden."},
    ShellStringEntry{IDS_ERR_NO_VOLUMES, L"No volumes found in this archive.",
                     L"\u6b64\u5f52\u6863\u4e2d\u672a\u627e\u5230\u5377\u3002", L"\u6b64\u5c01\u5b58\u6a94\u4e2d\u627e\u4e0d\u5230\u78c1\u789f\u5340\u3002",
                     L"\u3053\u306e\u30a2\u30fc\u30ab\u30a4\u30d6\u306b\u30dc\u30ea\u30e5\u30fc\u30e0\u304c\u898b\u3064\u304b\u308a\u307e\u305b\u3093\u3002",
                     L"In diesem Archiv wurden keine Volumes gefunden."},
    ShellStringEntry{IDS_ERR_PATH_EMPTY, L"Archive path is empty.",
                     L"\u5f52\u6863\u8def\u5f84\u4e3a\u7a7a\u3002", L"\u5c01\u5b58\u6a94\u8def\u5f91\u70ba\u7a7a\u3002",
                     L"\u30a2\u30fc\u30ab\u30a4\u30d6 \u30d1\u30b9\u304c\u7a7a\u3067\u3059\u3002",
                     L"Archivpfad ist leer."},
    ShellStringEntry{IDS_ERR_NTFS_PARSE, L"Unable to parse NTFS volume.",
                     L"\u65e0\u6cd5\u89e3\u6790 NTFS \u5377\u3002", L"\u7121\u6cd5\u89e3\u6790 NTFS \u78c1\u789f\u5340\u3002",
                     L"NTFS \u30dc\u30ea\u30e5\u30fc\u30e0\u3092\u89e3\u6790\u3067\u304d\u307e\u305b\u3093\u3002",
                     L"NTFS-Volume kann nicht analysiert werden."},
    ShellStringEntry{IDS_ERR_NTFS_ONLY, L"Only NTFS volumes can be browsed in Explorer.",
                     L"Explorer \u4e2d\u4ec5\u53ef\u6d4f\u89c8 NTFS \u5377\u3002",
                     L"Explorer \u4e2d\u50c5\u53ef\u700f\u89bd NTFS \u78c1\u789f\u5340\u3002",
                     L"Explorer \u3067\u53c2\u7167\u3067\u304d\u308b\u306e\u306f NTFS \u30dc\u30ea\u30e5\u30fc\u30e0\u306e\u307f\u3067\u3059\u3002",
                     L"In Explorer k\u00f6nnen nur NTFS-Volumes durchsucht werden."},
    ShellStringEntry{IDS_DISK_FORMAT, L"Disk %1",
                     L"\u78c1\u76d8 %1", L"\u78c1\u789f %1", L"\u30c7\u30a3\u30b9\u30af %1", L"Datentr\u00e4ger %1"},
    ShellStringEntry{IDS_VOLUME_FORMAT, L"Volume %1",
                     L"\u5377 %1", L"\u78c1\u789f\u5340 %1", L"\u30dc\u30ea\u30e5\u30fc\u30e0 %1", L"Volume %1"},
    ShellStringEntry{IDS_TIP_ARCHIVE_FOLDER, L"Archive folder",
                     L"\u5f52\u6863\u6587\u4ef6\u5939", L"\u5c01\u5b58\u6a94\u8cc7\u6599\u593e",
                     L"\u30a2\u30fc\u30ab\u30a4\u30d6 \u30d5\u30a9\u30eb\u30c0\u30fc", L"Archivordner"},
    ShellStringEntry{IDS_TIP_ARCHIVE_FILE, L"Archive file",
                     L"\u5f52\u6863\u6587\u4ef6", L"\u5c01\u5b58\u6a94\u6a94\u6848",
                     L"\u30a2\u30fc\u30ab\u30a4\u30d6 \u30d5\u30a1\u30a4\u30eb", L"Archivdatei"},
    ShellStringEntry{IDS_TIP_SIZE, L"Size: %1 bytes",
                     L"\u5927\u5c0f\uff1a%1 \u5b57\u8282", L"\u5927\u5c0f\uff1a%1 \u4f4d\u5143\u7d44",
                     L"\u30b5\u30a4\u30ba: %1 \u30d0\u30a4\u30c8", L"Gr\u00f6\u00dfe: %1 Bytes"},
    ShellStringEntry{IDS_TIP_SOURCE, L"Source: %1",
                     L"\u6765\u6e90\uff1a%1", L"\u4f86\u6e90\uff1a%1", L"\u30bd\u30fc\u30b9: %1", L"Quelle: %1"},
    ShellStringEntry{IDS_COLUMN_DATE_MODIFIED, L"Date modified",
                     L"\u4fee\u6539\u65e5\u671f", L"\u4fee\u6539\u65e5\u671f",
                     L"\u66f4\u65b0\u65e5\u6642", L"\u00c4nderungsdatum"},
    ShellStringEntry{IDS_COLUMN_TYPE, L"Type",
                     L"\u7c7b\u578b", L"\u985e\u578b", L"\u7a2e\u985e", L"Typ"},
    ShellStringEntry{IDS_OPEN_ITEM_FAILED, L"Failed to open item: %1",
                     L"\u65e0\u6cd5\u6253\u5f00\u9879\uff1a%1", L"\u7121\u6cd5\u958b\u555f\u9805\u76ee\uff1a%1",
                     L"\u9805\u76ee\u3092\u958b\u3051\u307e\u305b\u3093: %1",
                     L"Element konnte nicht ge\u00f6ffnet werden: %1"},
    ShellStringEntry{IDS_TYPE_DISK, L"Disk",
                     L"\u78c1\u76d8", L"\u78c1\u789f", L"\u30c7\u30a3\u30b9\u30af", L"Datentr\u00e4ger"},
    ShellStringEntry{IDS_TYPE_VOLUME, L"Volume",
                     L"\u5377", L"\u78c1\u789f\u5340", L"\u30dc\u30ea\u30e5\u30fc\u30e0", L"Volume"},
    ShellStringEntry{IDS_TYPE_FILE_FOLDER, L"File folder",
                     L"\u6587\u4ef6\u5939", L"\u8cc7\u6599\u593e", L"\u30d5\u30a1\u30a4\u30eb \u30d5\u30a9\u30eb\u30c0\u30fc",
                     L"Dateiordner"},
    ShellStringEntry{IDS_TYPE_FILE, L"File",
                     L"\u6587\u4ef6", L"\u6a94\u6848", L"\u30d5\u30a1\u30a4\u30eb", L"Datei"},
    ShellStringEntry{IDS_PASSWORD_CAPTION_DEFAULT, L"Password required",
                     L"\u9700\u8981\u5bc6\u7801", L"\u9700\u8981\u5bc6\u78bc",
                     L"\u30d1\u30b9\u30ef\u30fc\u30c9\u304c\u5fc5\u8981\u3067\u3059", L"Kennwort erforderlich"},
    ShellStringEntry{IDS_ERR_FILE_TOO_LARGE,
                     L"This file is larger than 1 GB and cannot be opened directly from the archive.\n"
                     L"Copy it out of the archive first, then open the copy.",
                     L"\u8be5\u6587\u4ef6\u8d85\u8fc7 1 GB\uff0c\u65e0\u6cd5\u4ece\u5f52\u6863\u4e2d\u76f4\u63a5\u6253\u5f00\u3002\n"
                     L"\u8bf7\u5148\u5c06\u6587\u4ef6\u590d\u5236\u51fa\u5f52\u6863\uff0c\u518d\u6253\u5f00\u590d\u5236\u540e\u7684\u6587\u4ef6\u3002",
                     L"\u8a72\u6a94\u6848\u8d85\u904e 1 GB\uff0c\u7121\u6cd5\u5f9e\u5c01\u5b58\u6a94\u4e2d\u76f4\u63a5\u958b\u555f\u3002\n"
                     L"\u8acb\u5148\u5c07\u6a94\u6848\u8907\u88fd\u51fa\u5c01\u5b58\u6a94\uff0c\u518d\u958b\u555f\u8907\u88fd\u5f8c\u7684\u6a94\u6848\u3002",
                     L"\u3053\u306e\u30d5\u30a1\u30a4\u30eb\u306f 1 GB \u3092\u8d85\u3048\u308b\u305f\u3081\u3001\u30a2\u30fc\u30ab\u30a4\u30d6\u304b\u3089\u76f4\u63a5\u958b\u3051\u307e\u305b\u3093\u3002\n"
                     L"\u307e\u305a\u30a2\u30fc\u30ab\u30a4\u30d6\u304b\u3089\u30b3\u30d4\u30fc\u3057\u3066\u304b\u3089\u958b\u3044\u3066\u304f\u3060\u3055\u3044\u3002",
                     L"Diese Datei ist gr\u00f6\u00dfer als 1 GB und kann nicht direkt aus dem Archiv ge\u00f6ffnet werden.\n"
                     L"Kopieren Sie die Datei zuerst aus dem Archiv und \u00f6ffnen Sie die Kopie."},
};
// clang-format on

[[nodiscard]] const wchar_t* pick_text(const ShellStringEntry& entry, const ShellUiLang lang) noexcept {
    switch (lang) {
    case ShellUiLang::ZhCn:
        return entry.zh_cn;
    case ShellUiLang::ZhTw:
        return entry.zh_tw;
    case ShellUiLang::Ja:
        return entry.ja;
    case ShellUiLang::De:
        return entry.de;
    case ShellUiLang::En:
    default:
        return entry.en;
    }
}

[[nodiscard]] std::wstring load_from_module(const unsigned id) {
    wchar_t buffer[512]{};
    const HINSTANCE instance = ATL::_AtlBaseModule.GetResourceInstance();
    const int length = ::LoadStringW(instance, id, buffer, static_cast<int>(std::size(buffer)));
    if (length <= 0) {
        return {};
    }
    return std::wstring(buffer, static_cast<std::size_t>(length));
}

[[nodiscard]] std::wstring replace_placeholder(std::wstring text, const std::wstring_view arg) {
    const auto pos = text.find(L"%1");
    if (pos == std::wstring::npos) {
        return text;
    }
    text.replace(pos, 2, arg);
    return text;
}

} // namespace

std::wstring load_shell_string(const unsigned id) {
    const ShellUiLang lang = detect_shell_ui_lang();
    for (const auto& entry : kShellStrings) {
        if (entry.id != id) {
            continue;
        }
        const wchar_t* text = pick_text(entry, lang);
        if (text == nullptr || text[0] == L'\0') {
            text = entry.en;
        }
        return text != nullptr ? std::wstring(text) : std::wstring{};
    }
    return load_from_module(id);
}

std::wstring format_shell_string(const unsigned id, const std::wstring_view arg1) {
    return replace_placeholder(load_shell_string(id), arg1);
}

std::wstring format_shell_string_u32(const unsigned id, const std::uint32_t value) {
    return format_shell_string(id, std::to_wstring(value));
}

} // namespace aegra::shell
