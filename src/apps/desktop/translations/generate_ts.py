# -*- coding: utf-8 -*-
from pathlib import Path

entries = [
    ("aegra.app.title", "Aegra", "Aegra", "Aegra", "Aegra", "Aegra"),
    ("aegra.nav.home", "Home", "主页", "首頁", "ホーム", "Startseite"),
    ("aegra.nav.backup", "Backup", "备份", "備份", "バックアップ", "Sicherung"),
    ("aegra.nav.restore", "Restore", "恢复", "還原", "復元", "Wiederherstellen"),
    ("aegra.nav.mount", "Mount", "挂载", "掛載", "マウント", "Einbinden"),
    ("aegra.nav.repository", "Repository", "Repository", "Repository", "リポジトリ", "Repository"),
    ("aegra.nav.event_log", "Event Log", "事件日志", "事件記錄", "イベントログ", "Ereignisprotokoll"),
    ("aegra.nav.settings", "Settings", "设置", "設定", "設定", "Einstellungen"),
    ("aegra.nav.feedback", "Feedback", "反馈", "意見回饋", "フィードバック", "Feedback"),
    (
        "aegra.nav.collapse_sidebar",
        "Collapse menu",
        "收起菜单",
        "收合選單",
        "メニューを折りたたむ",
        "Menü einklappen",
    ),
    (
        "aegra.nav.expand_sidebar",
        "Expand menu",
        "展开菜单",
        "展開選單",
        "メニューを展開",
        "Menü ausklappen",
    ),
    ("aegra.shell.service_label", "Service %1", "Service %1", "Service %1", "Service %1", "Service %1"),
    ("aegra.shell.language", "Language", "语言", "語言", "言語", "Sprache"),
    ("aegra.service.state.disconnected", "Disconnected", "未连接", "未連線", "未接続", "Getrennt"),
    (
        "aegra.service.state.connecting",
        "Connecting",
        "连接中",
        "連線中",
        "接続中",
        "Verbindung wird hergestellt",
    ),
    ("aegra.service.state.running", "Running", "运行中", "執行中", "実行中", "Läuft"),
    (
        "aegra.service.message.ready",
        "Service is ready",
        "Service 已就绪",
        "Service 已就緒",
        "Service の準備ができました",
        "Service ist bereit",
    ),
    ("aegra.common.unknown", "Unknown", "未知", "未知", "不明", "Unbekannt"),
    ("aegra.common.not_recorded", "Not recorded", "未记录", "未記錄", "未記録", "Nicht erfasst"),
    ("aegra.common.refresh", "Refresh", "刷新", "重新整理", "更新", "Aktualisieren"),
    ("aegra.common.reconnect", "Reconnect", "重新连接", "重新連線", "再接続", "Erneut verbinden"),
    ("aegra.common.add", "Add", "添加", "新增", "追加", "Hinzufügen"),
    ("aegra.common.import", "Import", "导入", "匯入", "インポート", "Importieren"),
    ("aegra.common.export", "Export", "导出", "匯出", "エクスポート", "Exportieren"),
    ("aegra.common.delete", "Delete", "删除", "刪除", "削除", "Löschen"),
    (
        "aegra.error.unknown",
        "Unexpected service response (%1)",
        "意外的 Service 响应（%1）",
        "意外的 Service 回應（%1）",
        "予期しない Service 応答（%1）",
        "Unerwartete Service-Antwort (%1)",
    ),
    (
        "aegra.error.repository.query_failed",
        "Unable to read the repository catalog",
        "无法读取 Repository 目录",
        "無法讀取 Repository 目錄",
        "リポジトリカタログを読み取れません",
        "Repository-Katalog konnte nicht gelesen werden",
    ),
    (
        "aegra.error.service.disconnected",
        "Service connection lost",
        "Service 连接已断开",
        "Service 連線已中斷",
        "Service 接続が切断されました",
        "Service-Verbindung getrennt",
    ),
    (
        "aegra.error.service.connect_failed",
        "Unable to connect to Service",
        "无法连接到 Service",
        "無法連線到 Service",
        "Service に接続できません",
        "Verbindung zum Service nicht möglich",
    ),
    (
        "aegra.error.service.protocol_invalid",
        "Invalid Service response",
        "Service 响应无效",
        "Service 回應無效",
        "Service 応答が無効です",
        "Ungültige Service-Antwort",
    ),
    (
        "aegra.error.service.request_timeout",
        "Service request timed out",
        "Service 请求超时",
        "Service 要求逾時",
        "Service 要求がタイムアウトしました",
        "Service-Anforderung zeitüberschritten",
    ),
    (
        "aegra.error.service.send_failed",
        "Failed to send Service request",
        "无法发送 Service 请求",
        "無法傳送 Service 要求",
        "Service 要求の送信に失敗しました",
        "Service-Anforderung konnte nicht gesendet werden",
    ),
    ("aegra.repository.title", "Repository", "Repository", "Repository", "リポジトリ", "Repository"),
    (
        "aegra.repository.personal_name",
        "Personal Repository",
        "个人版 Repository",
        "個人版 Repository",
        "個人リポジトリ",
        "Persönliches Repository",
    ),
    (
        "aegra.repository.recovery_points_count",
        "%1 recovery points",
        "%1 个恢复点",
        "%1 個還原點",
        "回復ポイント %1 件",
        "%1 Wiederherstellungspunkte",
    ),
    (
        "aegra.repository.kind_local_catalog",
        "local · catalog",
        "local · catalog",
        "local · catalog",
        "local · catalog",
        "local · catalog",
    ),
    (
        "aegra.repository.empty",
        "No repository",
        "没有 Repository",
        "沒有 Repository",
        "リポジトリがありません",
        "Kein Repository",
    ),
    (
        "aegra.repository.set_default",
        "Set default",
        "设为默认",
        "設為預設",
        "既定に設定",
        "Als Standard festlegen",
    ),
    (
        "aegra.repository.test_connection",
        "Test connection",
        "连接测试",
        "連線測試",
        "接続テスト",
        "Verbindung testen",
    ),
    ("aegra.repository.unlock", "Unlock", "解锁", "解除鎖定", "ロック解除", "Entsperren"),
    ("aegra.repository.lock", "Lock", "锁定", "鎖定", "ロック", "Sperren"),
    (
        "aegra.repository.rebuild_index",
        "Rebuild index",
        "重建索引",
        "重建索引",
        "インデックスを再構築",
        "Index neu erstellen",
    ),
    (
        "aegra.repository.set_password",
        "Set password",
        "设置密码",
        "設定密碼",
        "パスワード設定",
        "Kennwort festlegen",
    ),
    (
        "aegra.repository.drawer_title",
        "Personal Repository — Recovery Points",
        "个人版 Repository — 恢复点",
        "個人版 Repository — 還原點",
        "個人リポジトリ — 回復ポイント",
        "Persönliches Repository — Wiederherstellungspunkte",
    ),
    (
        "aegra.repository.column.recovery_point",
        "Recovery point",
        "恢复点",
        "還原點",
        "回復ポイント",
        "Wiederherstellungspunkt",
    ),
    (
        "aegra.repository.column.backup_time",
        "Backup time",
        "备份时间",
        "備份時間",
        "バックアップ時刻",
        "Sicherungszeit",
    ),
    ("aegra.repository.column.type", "Type", "类型", "類型", "種類", "Typ"),
    (
        "aegra.repository.column.logical_size",
        "Logical size",
        "逻辑大小",
        "邏輯大小",
        "論理サイズ",
        "Logische Größe",
    ),
    (
        "aegra.repository.column.stored_size",
        "Stored size",
        "存储大小",
        "儲存大小",
        "保存サイズ",
        "Gespeicherte Größe",
    ),
    (
        "aegra.repository.column.chain",
        "Backup chain",
        "备份链",
        "備份鏈",
        "バックアップチェーン",
        "Sicherungskette",
    ),
    (
        "aegra.repository.empty_recovery_points",
        "No recovery points",
        "没有恢复点",
        "沒有還原點",
        "回復ポイントがありません",
        "Keine Wiederherstellungspunkte",
    ),
    (
        "aegra.repository.status.waiting_service",
        "Waiting for Service",
        "等待 Service",
        "等待 Service",
        "Service を待機中",
        "Warte auf Service",
    ),
    (
        "aegra.repository.status.loading",
        "Reading catalog",
        "正在读取目录",
        "正在讀取目錄",
        "カタログを読み込み中",
        "Katalog wird gelesen",
    ),
    (
        "aegra.repository.status.read_failed",
        "Catalog read failed",
        "目录读取失败",
        "目錄讀取失敗",
        "カタログの読み込みに失敗しました",
        "Katalog konnte nicht gelesen werden",
    ),
    (
        "aegra.repository.status.catalog_ready",
        "Catalog available",
        "目录可用",
        "目錄可用",
        "カタログ利用可能",
        "Katalog verfügbar",
    ),
    (
        "aegra.repository.status.not_configured",
        "Not configured",
        "未配置",
        "未設定",
        "未構成",
        "Nicht konfiguriert",
    ),
    ("aegra.repository.chain.complete", "Complete", "完整", "完整", "完全", "Vollständig"),
    ("aegra.repository.chain.incomplete", "Incomplete", "不完整", "不完整", "不完全", "Unvollständig"),
    ("aegra.backup.type.full", "Full", "全量", "完整", "フル", "Vollständig"),
    ("aegra.backup.type.incremental", "Incremental", "增量", "遞增", "増分", "Inkrementell"),
    ("aegra.backup.type.differential", "Differential", "差异", "差異", "差分", "Differenziell"),
    ("aegra.backup.type.unknown", "Unknown", "未知", "未知", "不明", "Unbekannt"),
]


def esc(value: str) -> str:
    return (
        value.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def main() -> None:
    out_dir = Path(__file__).resolve().parent
    languages = [
        ("en_US", 0),
        ("zh_CN", 1),
        ("zh_TW", 2),
        ("ja_JP", 3),
        ("de_DE", 4),
    ]
    for lang_code, index in languages:
        lines = [
            '<?xml version="1.0" encoding="utf-8"?>',
            "<!DOCTYPE TS>",
            f'<TS version="2.1" language="{lang_code}">',
            "<context>",
            "    <name></name>",
        ]
        for row in entries:
            message_id = row[0]
            source = row[1]
            translation = row[1 + index]
            lines.append(f'    <message id="{esc(message_id)}">')
            lines.append(f"        <source>{esc(source)}</source>")
            lines.append(f"        <translation>{esc(translation)}</translation>")
            lines.append("    </message>")
        lines.append("</context>")
        lines.append("</TS>")
        path = out_dir / f"aegra_desktop_{lang_code}.ts"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"wrote {path.name} ({len(entries)} messages)")


if __name__ == "__main__":
    main()
