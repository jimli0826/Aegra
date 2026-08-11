pragma Singleton
import QtQuick 2.15

/**
 * App color theme — glass / dark / light palettes.
 * Colors are writable so setTheme() updates the whole UI via bindings to Theme.*.
 * Persistence is via localeController (C++ QSettings ui/theme), not Qt.labs.settings.
 *
 * Theme ids (stable for QSettings): blueExtra (default glass), dark, light.
 * colorTextWhite is the primary text color (may be dark on light/glass themes).
 */
QtObject {
    id: root

    // blueExtra (glass default) | dark | light
    property string themeId: "blueExtra"

    // ---- Live palette (bound by UI); defaults match glass (blueExtra) ----
    // Page bg matches sidebar so shell looks one continuous surface.
    property color colorBg: "#DFECE9"
    property color colorBgEnd: "#C8DFDC"
    property color colorCard: "#EFF7F5"
    property color colorCardEnd: "#E4F2EF"
    property color colorBorder: Qt.rgba(1.0, 1.0, 1.0, 0.90)
    property color colorCardShadow: Qt.rgba(0.08, 0.22, 0.24, 0.16)
    property color colorHeader: "#DFECE9"
    property color colorSidebar: "#DCE8EA"
    property color colorSidebarDivider: Qt.rgba(0.08, 0.22, 0.24, 0.15)
    /// Outer window chrome radius (0 when maximized). Tightened to 14 for clean DWM window corners.
    property int radiusWindow: 14
    property color colorTableHeader: "#CBE0E3"
    property color colorTableRow: "#DBE9EA"
    property color colorTableAlt: "#D2E3E5"
    property color colorPopup: "#EFF7F5"
    property color colorInput: "#ffffff"
    property color colorListItem: "#CCE0E2"
    property color colorListItemAlt: "#DBE9EA"

    property color colorAccentRed: "#FF6F85"
    property color colorAccentBlue: "#248894"
    property color colorGreen: "#2CB378"
    // Nav active: CoachPro / index.html primary teal gradient ends
    property color colorMenuActive: "#3798A3"
    property color colorMenuActiveEnd: "#237A85"
    property color colorMenuActiveText: "#ffffff"
    // Nav idle / hover (index.html .nav-item)
    property color colorMenuIdle: "#4D7577"
    property color colorMenuHoverText: "#122C30"
    property color colorMenuHoverBg: "#C4DADB"
    property color colorOnAccent: "#ffffff"
    property color colorLinkHover: "#186770"

    property color colorTextWhite: "#122C30"
    property color colorTextGrey: "#587E7B"
    property color colorTextDim: "#7D9F9C"

    property color colorHover: "#CBE0E2"
    property color colorHoverClose: "#FF6F85"

    property color colorButton: "#C4D8DA"
    property color colorButtonHover: "#B4CDD0"
    property color colorButtonDisabled: "#D6E4E6"
    property color colorButtonDisabledText: "#8EA5A8"
    property color colorProgressTrack: "#BDD1D4"
    property color colorCalendarMuted: "#8EA5A8"
    property color colorCalendarHasBackup: "#CCE0E2"

    property color colorToastSuccessBg: "#e5f6ed"
    property color colorToastSuccessBorder: "#3db87a"
    property color colorToastErrorBg: "#fdecea"
    property color colorToastErrorBorder: "#ef7d78"
    property color colorScrim: "#66000000"

    property var volumeColors: [
        "#a8d8ce", "#9bbfe0", "#d4bc8e", "#b0a8e0",
        "#f0a8c4", "#8cc8e0", "#7accc8", "#f0c490"
    ]
    property color colorVolumeText: "#1a2f2c"
    property color colorUnallocated: "#e8eeec"
    property color colorUnallocatedHatch: "#c5d4d0"
    property color colorUnallocatedText: "#7a9190"

    // Shared chrome metrics (concept: soft glass cards / pill nav)
    property int radiusCard: 18
    property int radiusControl: 10
    property int radiusMenu: 14
    property int radiusButton: 10

    function volumeColor(index) {
        var list = volumeColors
        if (!list || list.length === 0)
            return colorButton
        var i = index % list.length
        if (i < 0)
            i += list.length
        return list[i]
    }

    property string fontFamily: "Segoe UI"

    /// Catalog for Settings UI (preview chips) — glass (blueExtra) first (default)
    readonly property var themes: [
        {
            id: "blueExtra",
            labelKey: "aegra.settings.theme.blue_extra",
            previewBg: "#C4D6D8",
            previewCard: "#DBE9EA",
            previewAccent: "#3798A3",
            previewText: "#122C30"
        },
        {
            id: "oceanBlue",
            labelKey: "aegra.settings.theme.ocean_blue",
            previewBg: "#EBF3FC",
            previewCard: "#ECF3FD",
            previewAccent: "#3B82F6",
            previewText: "#1E293B"
        },
        {
            id: "dark",
            labelKey: "aegra.settings.theme.dark",
            previewBg: "#1e1e1e",
            previewCard: "#2d2d30",
            previewAccent: "#007acc",
            previewText: "#ffffff"
        },
        {
            id: "light",
            labelKey: "aegra.settings.theme.light",
            previewBg: "#F1F5F9",
            previewCard: "#ffffff",
            previewAccent: "#0078d4",
            previewText: "#0F172A"
        }
    ]

    readonly property var _palettes: ({
        "dark": {
            colorBg: "#1c1d22",
            colorBgEnd: "#141518",
            colorCard: "#22242c",
            colorCardEnd: "#1a1b22",
            colorBorder: "#363942",
            colorCardShadow: Qt.rgba(0, 0, 0, 0.40),
            colorHeader: "#1c1d22",
            colorSidebar: "#1c1d22",
            colorSidebarDivider: "#363942",
            colorTableHeader: "#2a2d36",
            colorTableRow: "#24262d",
            colorTableAlt: "#272931",
            colorPopup: "#272932",
            colorInput: "#1b1c20",
            colorListItem: "#2a2d36",
            colorListItemAlt: "#24262d",
            colorAccentRed: "#ef4444",
            colorAccentBlue: "#38bdf8",
            colorGreen: "#10b981",
            colorMenuActive: "#1e3a5f",
            colorMenuActiveEnd: "#162a45",
            colorMenuActiveText: "#ffffff",
            colorMenuIdle: "#8b949e",
            colorMenuHoverText: "#38bdf8",
            colorMenuHoverBg: "#252832",
            colorOnAccent: "#ffffff",
            colorLinkHover: "#38bdf8",
            colorTextWhite: "#f1f3f7",
            colorTextGrey: "#9ba3af",
            colorTextDim: "#6b7280",
            colorHover: "#2e313b",
            colorHoverClose: "#ef4444",
            colorButton: "#2e313b",
            colorButtonHover: "#3b3f4c",
            colorButtonDisabled: "#202227",
            colorButtonDisabledText: "#555b66",
            colorProgressTrack: "#2e313b",
            colorCalendarMuted: "#6b7280",
            colorCalendarHasBackup: "#1e3a5f",
            colorToastSuccessBg: "#064e3b",
            colorToastSuccessBorder: "#10b981",
            colorToastErrorBg: "#450a0a",
            colorToastErrorBorder: "#ef4444",
            colorScrim: "#99000000",
            volumeColors: [
                "#4a90c8", "#5ba3d9", "#3d7aab", "#6bb3e0",
                "#2e6a9e", "#7ec0e8", "#3a8fc4", "#568fb5"
            ],
            colorVolumeText: "#ffffff",
            colorUnallocated: "#24262d",
            colorUnallocatedHatch: "#363942",
            colorUnallocatedText: "#9ba3af",
            radiusCard: 16,
            radiusControl: 8,
            radiusMenu: 10,
            radiusButton: 8
        },
        "oceanBlue": {
            // Coachwyse / Ocean Blue glass theme matching user image
            colorBg: "#EBF3FC",
            colorBgEnd: "#E3EEFB",
            colorCard: "#F5F9FE",
            colorCardEnd: "#EBF3FC",
            colorBorder: "#ffffff",
            colorCardShadow: Qt.rgba(0.08, 0.18, 0.35, 0.10),
            colorHeader: "#EBF3FC",
            colorSidebar: "#EBF3FC",
            colorSidebarDivider: Qt.rgba(0.10, 0.20, 0.40, 0.12),
            colorTableHeader: "#DEECFC",
            colorTableRow: "#F5F9FE",
            colorTableAlt: "#EBF3FC",
            colorPopup: "#ffffff",
            colorInput: "#ffffff",
            colorListItem: "#E1EEFC",
            colorListItemAlt: "#F5F9FE",
            colorAccentRed: "#EF4444",
            colorAccentBlue: "#2563EB",
            colorGreen: "#10B981",
            colorMenuActive: "#2E64D8",
            colorMenuActiveEnd: "#1D4ED8",
            colorMenuActiveText: "#ffffff",
            colorMenuIdle: "#5C769D",
            colorMenuHoverText: "#1D4ED8",
            colorMenuHoverBg: "#DBEAFE",
            colorOnAccent: "#ffffff",
            colorLinkHover: "#1D4ED8",
            colorTextWhite: "#0F172A",
            colorTextGrey: "#475569",
            colorTextDim: "#64748B",
            colorHover: "#DFEDFC",
            colorHoverClose: "#EF4444",
            colorButton: "#D8E8FC",
            colorButtonHover: "#C6DCFA",
            colorButtonDisabled: "#F1F5F9",
            colorButtonDisabledText: "#94A3B8",
            colorProgressTrack: "#D8E8FC",
            colorCalendarMuted: "#64748B",
            colorCalendarHasBackup: "#DBEAFE",
            colorToastSuccessBg: "#ECFDF5",
            colorToastSuccessBorder: "#10B981",
            colorToastErrorBg: "#FEF2F2",
            colorToastErrorBorder: "#EF4444",
            colorScrim: "#66000000",
            volumeColors: [
                "#2563EB", "#6366F1", "#0EA5E9", "#10B981",
                "#EC4899", "#8B5CF6", "#14B8A6", "#F59E0B"
            ],
            colorVolumeText: "#0F172A",
            colorUnallocated: "#E2E8F0",
            colorUnallocatedHatch: "#CBD5E1",
            colorUnallocatedText: "#64748B",
            radiusCard: 18,
            radiusControl: 10,
            radiusMenu: 14,
            radiusButton: 10
        },
        "blueExtra": {
            // Glass / CoachPro-inspired mint-teal (concept background)
            colorBg: "#DFECE9",
            colorBgEnd: "#C8DFDC",
            colorCard: "#EFF7F5",
            colorCardEnd: "#E4F2EF",
            colorBorder: Qt.rgba(1.0, 1.0, 1.0, 0.90),
            colorCardShadow: Qt.rgba(0.08, 0.22, 0.24, 0.16),
            colorHeader: "#DFECE9",
            colorSidebar: "#DCE8EA",
            colorSidebarDivider: Qt.rgba(0.08, 0.22, 0.24, 0.15),
            colorTableHeader: "#CBE0E3",
            colorTableRow: "#DBE9EA",
            colorTableAlt: "#D2E3E5",
            colorPopup: "#EFF7F5",
            colorInput: "#ffffff",
            colorListItem: "#CCE0E2",
            colorListItemAlt: "#DBE9EA",
            colorAccentRed: "#FF6F85",
            colorAccentBlue: "#248894",
            colorGreen: "#2CB378",
            colorMenuActive: "#3798A3",
            colorMenuActiveEnd: "#237A85",
            colorMenuActiveText: "#ffffff",
            colorMenuIdle: "#4D7577",
            colorMenuHoverText: "#122C30",
            colorMenuHoverBg: "#C4DADB",
            colorOnAccent: "#ffffff",
            colorLinkHover: "#186770",
            colorTextWhite: "#122C30",
            colorTextGrey: "#587E7B",
            colorTextDim: "#7D9F9C",
            colorHover: "#CBE0E2",
            colorHoverClose: "#FF6F85",
            colorButton: "#C4D8DA",
            colorButtonHover: "#B4CDD0",
            colorButtonDisabled: "#D6E4E6",
            colorButtonDisabledText: "#8EA5A8",
            colorProgressTrack: "#BDD1D4",
            colorCalendarMuted: "#8EA5A8",
            colorCalendarHasBackup: "#CCE0E2",
            colorToastSuccessBg: "#E5F6ED",
            colorToastSuccessBorder: "#2CB378",
            colorToastErrorBg: "#FDECEA",
            colorToastErrorBorder: "#FF6F85",
            colorScrim: "#66000000",
            volumeColors: [
                "#7fc4b4", "#6a9fd4", "#c4a06a", "#8b7fd9",
                "#e87ba8", "#5ba8c9", "#3ab0a8", "#e9a85c"
            ],
            colorVolumeText: "#122C30",
            colorUnallocated: "#C8DADC",
            colorUnallocatedHatch: "#A8C4C7",
            colorUnallocatedText: "#587E7B",
            radiusCard: 20,
            radiusControl: 10,
            radiusMenu: 14,
            radiusButton: 10
        },
        "light": {
            // Modern Clean Light theme (Fluent / Slate)
            colorBg: "#F1F5F9",
            colorBgEnd: "#E0EFFE",
            colorCard: "#ffffff",
            colorCardEnd: "#EEF5FD",
            colorBorder: "#CBD5E1",
            colorCardShadow: Qt.rgba(0.08, 0.12, 0.20, 0.08),
            colorHeader: "#F1F5F9",
            colorSidebar: "#F1F5F9",
            colorSidebarDivider: "#E2E8F0",
            colorTableHeader: "#E2E8F0",
            colorTableRow: "#ffffff",
            colorTableAlt: "#F8FAFC",
            colorPopup: "#ffffff",
            colorInput: "#ffffff",
            colorListItem: "#F1F5F9",
            colorListItemAlt: "#ffffff",
            colorAccentRed: "#EF4444",
            colorAccentBlue: "#0078D4",
            colorGreen: "#10B981",
            colorMenuActive: "#0078D4",
            colorMenuActiveEnd: "#005A9E",
            colorMenuActiveText: "#ffffff",
            colorMenuIdle: "#64748B",
            colorMenuHoverText: "#0F172A",
            colorMenuHoverBg: "#E2E8F0",
            colorOnAccent: "#ffffff",
            colorLinkHover: "#005A9E",
            colorTextWhite: "#0F172A",
            colorTextGrey: "#475569",
            colorTextDim: "#94A3B8",
            colorHover: "#E2E8F0",
            colorHoverClose: "#EF4444",
            colorButton: "#E2E8F0",
            colorButtonHover: "#CBD5E1",
            colorButtonDisabled: "#F1F5F9",
            colorButtonDisabledText: "#94A3B8",
            colorProgressTrack: "#E2E8F0",
            colorCalendarMuted: "#94A3B8",
            colorCalendarHasBackup: "#E0F2FE",
            colorToastSuccessBg: "#ECFDF5",
            colorToastSuccessBorder: "#10B981",
            colorToastErrorBg: "#FEF2F2",
            colorToastErrorBorder: "#EF4444",
            colorScrim: "#66000000",
            volumeColors: [
                "#0078D4", "#6366F1", "#0EA5E9", "#10B981",
                "#EC4899", "#8B5CF6", "#14B8A6", "#F59E0B"
            ],
            colorVolumeText: "#0F172A",
            colorUnallocated: "#E2E8F0",
            colorUnallocatedHatch: "#CBD5E1",
            colorUnallocatedText: "#64748B",
            radiusCard: 18,
            radiusControl: 10,
            radiusMenu: 14,
            radiusButton: 10
        }
    })

    function setTheme(id) {
        if (!id || !_palettes[id])
            return
        themeId = id
        applyPalette(_palettes[id])
        // Persist like old systemBackend.setTheme via localeController (QSettings ui/theme)
        if (typeof localeController !== "undefined" && localeController
                && localeController.setTheme)
            localeController.setTheme(id)
    }

    function applyPalette(p) {
        colorBg = p.colorBg
        if (p.colorBgEnd !== undefined)
            colorBgEnd = p.colorBgEnd
        colorCard = p.colorCard
        if (p.colorCardEnd !== undefined)
            colorCardEnd = p.colorCardEnd
        else
            colorCardEnd = p.colorCard
        colorBorder = p.colorBorder
        if (p.colorCardShadow !== undefined)
            colorCardShadow = p.colorCardShadow
        colorHeader = p.colorHeader
        colorSidebar = p.colorSidebar
        if (p.colorSidebarDivider !== undefined)
            colorSidebarDivider = p.colorSidebarDivider
        colorTableHeader = p.colorTableHeader
        colorTableRow = p.colorTableRow
        colorTableAlt = p.colorTableAlt
        colorPopup = p.colorPopup
        colorInput = p.colorInput
        colorListItem = p.colorListItem
        colorListItemAlt = p.colorListItemAlt
        colorAccentRed = p.colorAccentRed
        colorAccentBlue = p.colorAccentBlue
        colorGreen = p.colorGreen
        colorMenuActive = p.colorMenuActive
        if (p.colorMenuActiveEnd !== undefined)
            colorMenuActiveEnd = p.colorMenuActiveEnd
        if (p.colorMenuActiveText !== undefined)
            colorMenuActiveText = p.colorMenuActiveText
        if (p.colorMenuIdle !== undefined)
            colorMenuIdle = p.colorMenuIdle
        if (p.colorMenuHoverText !== undefined)
            colorMenuHoverText = p.colorMenuHoverText
        if (p.colorMenuHoverBg !== undefined)
            colorMenuHoverBg = p.colorMenuHoverBg
        if (p.colorOnAccent !== undefined)
            colorOnAccent = p.colorOnAccent
        if (p.colorLinkHover !== undefined)
            colorLinkHover = p.colorLinkHover
        colorTextWhite = p.colorTextWhite
        colorTextGrey = p.colorTextGrey
        colorTextDim = p.colorTextDim
        colorHover = p.colorHover
        colorHoverClose = p.colorHoverClose
        colorButton = p.colorButton
        colorButtonHover = p.colorButtonHover
        colorButtonDisabled = p.colorButtonDisabled
        colorButtonDisabledText = p.colorButtonDisabledText
        colorProgressTrack = p.colorProgressTrack
        colorCalendarMuted = p.colorCalendarMuted
        colorCalendarHasBackup = p.colorCalendarHasBackup
        colorToastSuccessBg = p.colorToastSuccessBg
        colorToastSuccessBorder = p.colorToastSuccessBorder
        colorToastErrorBg = p.colorToastErrorBg
        colorToastErrorBorder = p.colorToastErrorBorder
        if (p.colorScrim !== undefined)
            colorScrim = p.colorScrim
        volumeColors = p.volumeColors
        colorVolumeText = p.colorVolumeText
        if (p.colorUnallocated !== undefined)
            colorUnallocated = p.colorUnallocated
        if (p.colorUnallocatedHatch !== undefined)
            colorUnallocatedHatch = p.colorUnallocatedHatch
        if (p.colorUnallocatedText !== undefined)
            colorUnallocatedText = p.colorUnallocatedText
        if (p.radiusCard !== undefined)
            radiusCard = p.radiusCard
        if (p.radiusControl !== undefined)
            radiusControl = p.radiusControl
        if (p.radiusMenu !== undefined)
            radiusMenu = p.radiusMenu
        if (p.radiusButton !== undefined)
            radiusButton = p.radiusButton
    }

    function initFromBackend() {
        // Same pattern as old Theme.initFromBackend() reading systemBackend.theme
        if (typeof localeController !== "undefined" && localeController
                && localeController.theme) {
            var id = localeController.theme
            if (_palettes[id]) {
                themeId = id
                applyPalette(_palettes[id])
            }
        }
    }

    function themeLabel(item) {
        if (!item)
            return ""
        if (item.labelKey)
            return qsTrId(item.labelKey)
        return item.id || ""
    }
}
