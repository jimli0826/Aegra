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
    property color colorBg: "#eef7f5"
    property color colorBgEnd: "#eef7f5"
    property color colorCard: "#f7fbfa"
    property color colorBorder: "#c5d9d5"
    property color colorHeader: "#eef7f5"
    property color colorSidebar: "#eef7f5"
    /// Outer window chrome radius (0 when maximized). Concept mock uses ~28; keep a bit
    /// tighter so Windows DWM still looks clean with the MultiEffect mask.
    property int radiusWindow: 22
    property color colorTableHeader: "#e2f0ed"
    property color colorTableRow: "#f7fbfa"
    property color colorTableAlt: "#eef6f4"
    property color colorPopup: "#ffffff"
    property color colorInput: "#ffffff"
    property color colorListItem: "#e8f4f2"
    property color colorListItemAlt: "#f7fbfa"

    property color colorAccentRed: "#ef7d78"
    property color colorAccentBlue: "#2a9aa3"
    property color colorGreen: "#3db87a"
    // Nav active: CoachPro / index.html primary teal gradient ends
    property color colorMenuActive: "#2A7982"
    property color colorMenuActiveEnd: "#1E5C64"
    property color colorMenuActiveText: "#ffffff"
    // Nav idle / hover (index.html .nav-item)
    property color colorMenuIdle: "#557773"
    property color colorMenuHoverText: "#2A7982"
    property color colorMenuHoverBg: "#E4F1F2"
    property color colorOnAccent: "#ffffff"
    property color colorLinkHover: "#1f7a82"

    property color colorTextWhite: "#1a2f2c"
    property color colorTextGrey: "#5a7572"
    property color colorTextDim: "#8a9f9c"

    property color colorHover: "#E4F1F2"
    property color colorHoverClose: "#ef7d78"

    property color colorButton: "#e0eeeb"
    property color colorButtonHover: "#cfe4e0"
    property color colorButtonDisabled: "#eef4f3"
    property color colorButtonDisabledText: "#a0b5b2"
    property color colorProgressTrack: "#d5e8e4"
    property color colorCalendarMuted: "#a0b5b2"
    property color colorCalendarHasBackup: "#d0ece9"

    property color colorToastSuccessBg: "#e5f6ed"
    property color colorToastSuccessBorder: "#3db87a"
    property color colorToastErrorBg: "#fdecea"
    property color colorToastErrorBorder: "#ef7d78"
    property color colorScrim: "#66000000"

    property var volumeColors: [
        "#7fc4b4", "#6a9fd4", "#c4a06a", "#8b7fd9",
        "#e87ba8", "#5ba8c9", "#3ab0a8", "#e9a85c"
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
            previewBg: "#eef7f5",
            previewCard: "#f7fbfa",
            previewAccent: "#2a9aa3",
            previewText: "#1a2f2c"
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
            previewBg: "#eeeeee",
            previewCard: "#ffffff",
            previewAccent: "#0078d4",
            previewText: "#1e1e1e"
        }
    ]

    readonly property var _palettes: ({
        "dark": {
            colorBg: "#252526",
            colorBgEnd: "#1e1e1e",
            colorCard: "#2d2d30",
            colorBorder: "#3f3f46",
            colorHeader: "#2d2d30",
            colorSidebar: "#252526",
            colorTableHeader: "#333337",
            colorTableRow: "#2d2d30",
            colorTableAlt: "#38383d",
            colorPopup: "#2d2d30",
            colorInput: "#1e1e1e",
            colorListItem: "#333337",
            colorListItemAlt: "#2a2a2e",
            colorAccentRed: "#e81123",
            colorAccentBlue: "#00a8ff",
            colorGreen: "#00cc66",
            colorMenuActive: "#0e639c",
            colorMenuActiveText: "#ffffff",
            colorOnAccent: "#ffffff",
            colorLinkHover: "#33b8ff",
            colorTextWhite: "#f3f3f3",
            colorTextGrey: "#a0a0a0",
            colorTextDim: "#6e6e6e",
            colorHover: "#3e3e42",
            colorHoverClose: "#e81123",
            colorButton: "#3e3e42",
            colorButtonHover: "#4a4a4f",
            colorButtonDisabled: "#2a2a2e",
            colorButtonDisabledText: "#6e6e6e",
            colorProgressTrack: "#3e3e42",
            colorCalendarMuted: "#6e6e6e",
            colorCalendarHasBackup: "#3a3a40",
            colorToastSuccessBg: "#1a3328",
            colorToastSuccessBorder: "#00cc66",
            colorToastErrorBg: "#3a1e1e",
            colorToastErrorBorder: "#e81123",
            colorScrim: "#99000000",
            volumeColors: [
                "#4a90c8", "#5ba3d9", "#3d7aab", "#6bb3e0",
                "#2e6a9e", "#7ec0e8", "#3a8fc4", "#568fb5"
            ],
            colorVolumeText: "#ffffff",
            colorUnallocated: "#2a2a2e",
            colorUnallocatedHatch: "#4a4a50",
            colorUnallocatedText: "#9a9a9a",
            radiusCard: 8,
            radiusControl: 6,
            radiusMenu: 6,
            radiusButton: 6
        },
        "blueExtra": {
            // Glass / CoachPro-inspired mint-teal (default product look)
            colorBg: "#eef7f5",
            colorBgEnd: "#eef7f5",
            colorCard: "#f7fbfa",
            colorBorder: "#c5d9d5",
            colorHeader: "#eef7f5",
            colorSidebar: "#eef7f5",
            colorTableHeader: "#e2f0ed",
            colorTableRow: "#f7fbfa",
            colorTableAlt: "#eef6f4",
            colorPopup: "#ffffff",
            colorInput: "#ffffff",
            colorListItem: "#e8f4f2",
            colorListItemAlt: "#f7fbfa",
            colorAccentRed: "#ef7d78",
            colorAccentBlue: "#2a9aa3",
            colorGreen: "#3db87a",
            colorMenuActive: "#2A7982",
            colorMenuActiveEnd: "#1E5C64",
            colorMenuActiveText: "#ffffff",
            colorMenuIdle: "#557773",
            colorMenuHoverText: "#2A7982",
            colorMenuHoverBg: "#E4F1F2",
            colorOnAccent: "#ffffff",
            colorLinkHover: "#1f7a82",
            colorTextWhite: "#1a2f2c",
            colorTextGrey: "#5a7572",
            colorTextDim: "#8a9f9c",
            colorHover: "#E4F1F2",
            colorHoverClose: "#ef7d78",
            colorButton: "#e0eeeb",
            colorButtonHover: "#cfe4e0",
            colorButtonDisabled: "#eef4f3",
            colorButtonDisabledText: "#a0b5b2",
            colorProgressTrack: "#d5e8e4",
            colorCalendarMuted: "#a0b5b2",
            colorCalendarHasBackup: "#d0ece9",
            colorToastSuccessBg: "#e5f6ed",
            colorToastSuccessBorder: "#3db87a",
            colorToastErrorBg: "#fdecea",
            colorToastErrorBorder: "#ef7d78",
            colorScrim: "#66000000",
            volumeColors: [
                "#7fc4b4", "#6a9fd4", "#c4a06a", "#8b7fd9",
                "#e87ba8", "#5ba8c9", "#3ab0a8", "#e9a85c"
            ],
            colorVolumeText: "#1a2f2c",
            colorUnallocated: "#e8eeec",
            colorUnallocatedHatch: "#c5d4d0",
            colorUnallocatedText: "#7a9190",
            radiusCard: 14,
            radiusControl: 10,
            radiusMenu: 14,
            radiusButton: 10
        },
        "light": {
            colorBg: "#f0f0f0",
            colorBgEnd: "#e8e8ec",
            colorCard: "#ffffff",
            colorBorder: "#cccedb",
            colorHeader: "#f5f5f5",
            colorSidebar: "#e8e8ec",
            colorTableHeader: "#eef0f4",
            colorTableRow: "#ffffff",
            colorTableAlt: "#f5f6f8",
            colorPopup: "#ffffff",
            colorInput: "#ffffff",
            colorListItem: "#f7f7f9",
            colorListItemAlt: "#ffffff",
            colorAccentRed: "#e81123",
            colorAccentBlue: "#0078d4",
            colorGreen: "#107c10",
            colorMenuActive: "#cce4f7",
            colorMenuActiveText: "#1e1e1e",
            colorOnAccent: "#ffffff",
            colorLinkHover: "#005a9e",
            colorTextWhite: "#1e1e1e",
            colorTextGrey: "#616161",
            colorTextDim: "#8a8a8a",
            colorHover: "#e5f1fb",
            colorHoverClose: "#e81123",
            colorButton: "#e8e8ec",
            colorButtonHover: "#dcdce0",
            colorButtonDisabled: "#f0f0f0",
            colorButtonDisabledText: "#a0a0a0",
            colorProgressTrack: "#e0e0e4",
            colorCalendarMuted: "#a0a0a0",
            colorCalendarHasBackup: "#e8f0fa",
            colorToastSuccessBg: "#e6f4ea",
            colorToastSuccessBorder: "#107c10",
            colorToastErrorBg: "#fdecea",
            colorToastErrorBorder: "#e81123",
            colorScrim: "#66000000",
            volumeColors: [
                "#a8c4dc", "#9bb8d4", "#b5cfe6", "#8fafcc",
                "#c0d8ec", "#85a6c4", "#d0e4f2", "#7a9cba"
            ],
            colorVolumeText: "#1e2a38",
            colorUnallocated: "#e8eaed",
            colorUnallocatedHatch: "#c5cad3",
            colorUnallocatedText: "#616161",
            radiusCard: 8,
            radiusControl: 6,
            radiusMenu: 6,
            radiusButton: 6
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
        colorBorder = p.colorBorder
        colorHeader = p.colorHeader
        colorSidebar = p.colorSidebar
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
