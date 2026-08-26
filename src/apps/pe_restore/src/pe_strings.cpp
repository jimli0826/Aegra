#include "pe_strings.h"

#include <array>
#include <cstddef>

namespace aegra::apps::pe_restore {
namespace {

inline constexpr std::size_t kStringCount =
    static_cast<std::size_t>(StringId::kCancelDisabledNote) + 1;

struct LanguageTable final {
    std::string_view locale_prefix;
    std::array<const wchar_t*, kStringCount> strings;
};

inline constexpr LanguageTable kEnglish = {
    "en",
    {
        L"Aegra System Recovery",
        L"Preparing the recovery environment...",
        L"Locating the pending restore job...",
        L"Verifying the backup and the target disk...",
        L"Restoring the system disk. Do not power off the machine.",
        L"Restore finished.",
        L"The following restore will run:",
        L"Backup:",
        L"Target disk:",
        L"WARNING: every byte on the target disk will be overwritten. This cannot be undone.",
        L"Starting automatically in %u seconds...",
        L"Start now",
        L"Cancel and restart Windows",
        L"This backup is encrypted. Enter the backup password:",
        L"Continue",
        L"The system disk was restored successfully.",
        L"The restore failed. The target disk may be incomplete.",
        L"The restore was cancelled. No data was written.",
        L"Restarting in %u seconds...",
        L"%u%%  (%llu MB written)",
        L"Writing has started; cancelling is no longer safe.",
    }};

inline constexpr LanguageTable kSimplifiedChinese = {
    "zh-CN",
    {
        L"Aegra 系统恢复",
        L"正在准备恢复环境...",
        L"正在定位待执行的恢复任务...",
        L"正在校验备份与目标磁盘...",
        L"正在恢复系统盘，请勿断电。",
        L"恢复已完成。",
        L"即将执行以下恢复：",
        L"备份：",
        L"目标磁盘：",
        L"警告：目标磁盘上的所有数据将被覆盖，且不可撤销。",
        L"%u 秒后自动开始...",
        L"立即开始",
        L"取消并重启回 Windows",
        L"该备份已加密，请输入备份密码：",
        L"继续",
        L"系统盘已成功恢复。",
        L"恢复失败，目标磁盘可能不完整。",
        L"恢复已取消，未写入任何数据。",
        L"%u 秒后重新启动...",
        L"%u%%（已写入 %llu MB）",
        L"数据写入已开始，无法再安全取消。",
    }};

inline constexpr LanguageTable kTraditionalChinese = {
    "zh-TW",
    {
        L"Aegra 系統復原",
        L"正在準備復原環境...",
        L"正在定位待執行的復原工作...",
        L"正在驗證備份與目標磁碟...",
        L"正在復原系統磁碟，請勿關閉電源。",
        L"復原已完成。",
        L"即將執行以下復原：",
        L"備份：",
        L"目標磁碟：",
        L"警告：目標磁碟上的所有資料將被覆寫，且無法復原。",
        L"%u 秒後自動開始...",
        L"立即開始",
        L"取消並重新啟動 Windows",
        L"此備份已加密，請輸入備份密碼：",
        L"繼續",
        L"系統磁碟已成功復原。",
        L"復原失敗，目標磁碟可能不完整。",
        L"復原已取消，未寫入任何資料。",
        L"%u 秒後重新啟動...",
        L"%u%%（已寫入 %llu MB）",
        L"資料寫入已開始，無法再安全取消。",
    }};

inline constexpr LanguageTable kJapanese = {
    "ja",
    {
        L"Aegra システム復元",
        L"回復環境を準備しています...",
        L"保留中の復元ジョブを探しています...",
        L"バックアップと対象ディスクを検証しています...",
        L"システムディスクを復元しています。電源を切らないでください。",
        L"復元が完了しました。",
        L"次の復元を実行します：",
        L"バックアップ：",
        L"対象ディスク：",
        L"警告：対象ディスク上のすべてのデータが上書きされます。元に戻せません。",
        L"%u 秒後に自動的に開始します...",
        L"今すぐ開始",
        L"キャンセルして Windows を再起動",
        L"このバックアップは暗号化されています。パスワードを入力してください：",
        L"続行",
        L"システムディスクは正常に復元されました。",
        L"復元に失敗しました。対象ディスクは不完全な可能性があります。",
        L"復元はキャンセルされました。データは書き込まれていません。",
        L"%u 秒後に再起動します...",
        L"%u%%（%llu MB 書き込み済み）",
        L"書き込みが開始されたため、安全に中止できません。",
    }};

inline constexpr LanguageTable kGerman = {
    "de",
    {
        L"Aegra Systemwiederherstellung",
        L"Wiederherstellungsumgebung wird vorbereitet...",
        L"Ausstehender Wiederherstellungsauftrag wird gesucht...",
        L"Sicherung und Zieldatenträger werden geprüft...",
        L"Systemdatenträger wird wiederhergestellt. Gerät nicht ausschalten.",
        L"Wiederherstellung abgeschlossen.",
        L"Die folgende Wiederherstellung wird ausgeführt:",
        L"Sicherung:",
        L"Zieldatenträger:",
        L"WARNUNG: Alle Daten auf dem Zieldatenträger werden überschrieben. Dies kann nicht "
        L"rückgängig gemacht werden.",
        L"Automatischer Start in %u Sekunden...",
        L"Jetzt starten",
        L"Abbrechen und Windows neu starten",
        L"Diese Sicherung ist verschlüsselt. Kennwort eingeben:",
        L"Weiter",
        L"Der Systemdatenträger wurde erfolgreich wiederhergestellt.",
        L"Die Wiederherstellung ist fehlgeschlagen. Der Zieldatenträger ist eventuell "
        L"unvollständig.",
        L"Die Wiederherstellung wurde abgebrochen. Es wurden keine Daten geschrieben.",
        L"Neustart in %u Sekunden...",
        L"%u%% (%llu MB geschrieben)",
        L"Der Schreibvorgang hat begonnen; ein sicherer Abbruch ist nicht mehr möglich.",
    }};

[[nodiscard]] const LanguageTable& table_for(const std::string_view locale) {
    // zh-TW before the generic zh prefix; every other language matches by prefix.
    if (locale.starts_with("zh-TW") || locale.starts_with("zh-HK")) {
        return kTraditionalChinese;
    }
    if (locale.starts_with("zh")) {
        return kSimplifiedChinese;
    }
    if (locale.starts_with("ja")) {
        return kJapanese;
    }
    if (locale.starts_with("de")) {
        return kGerman;
    }
    return kEnglish;
}

} // namespace

const wchar_t* pe_string(const StringId id, const std::string_view locale) {
    const auto& table = table_for(locale);
    const auto index = static_cast<std::size_t>(id);
    if (index >= table.strings.size()) {
        return L"";
    }
    return table.strings[index];
}

} // namespace aegra::apps::pe_restore
