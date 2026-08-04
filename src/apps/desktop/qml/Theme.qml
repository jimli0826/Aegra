pragma Singleton
import QtQuick 2.15

/**
 * App color theme — Visual Studio–style palettes (same approach as backup/src/gui Theme.qml).
 * Colors are writable so setTheme() updates the whole UI via bindings to Theme.*.
 * Persistence is via localeController (C++ QSettings ui/theme), not Qt.labs.settings.
 */
QtObject {
    id: root

    // blueExtra | dark | light  — blueExtra is the default
    property string themeId: "blueExtra"

    // ---- Live palette (bound by UI); defaults match blueExtra ----
    property color colorBg: "#1e3048"
    property color colorCard: "#2a4260"
    property color colorBorder: "#4a6a90"
    property color colorHeader: "#243a56"
    property color colorSidebar: "#243a56"
    property color colorTableHeader: "#34506e"
    property color colorTableRow: "#2a4260"
    property color colorTableAlt: "#314a6a"
    property color colorPopup: "#2a4260"
    property color colorInput: "#243a56"
    property color colorListItem: "#34506e"
    property color colorListItemAlt: "#2a4260"

    property color colorAccentRed: "#ff5c5c"
    property color colorAccentBlue: "#4fc1ff"
    property color colorGreen: "#57d38c"
    property color colorMenuActive: "#4fc1ff"

    property color colorTextWhite: "#f0f4f8"
    property color colorTextGrey: "#b4c8dc"
    property color colorTextDim: "#8aa4c0"

    property color colorHover: "#3a5878"
    property color colorHoverClose: "#ff5c5c"

    property color colorButton: "#3a5574"
    property color colorButtonHover: "#466888"
    property color colorButtonDisabled: "#2d4560"
    property color colorButtonDisabledText: "#8aa4c0"
    property color colorProgressTrack: "#3a5574"
    property color colorCalendarMuted: "#8aa4c0"
    property color colorCalendarHasBackup: "#355878"

    property color colorToastSuccessBg: "#1e3d32"
    property color colorToastSuccessBorder: "#57d38c"
    property color colorToastErrorBg: "#3d2428"
    property color colorToastErrorBorder: "#ff5c5c"
    property color colorScrim: "#99000000"

    property var volumeColors: [
        "#4d6d8c", "#5a7a98", "#436384", "#6586a4",
        "#4a6a88", "#6e90ac", "#3f5e7e", "#7898b4"
    ]
    property color colorVolumeText: "#f0f4f8"
    property color colorUnallocated: "#252f3d"
    property color colorUnallocatedHatch: "#3d4a5c"
    property color colorUnallocatedText: "#9aabbc"

    property string fontFamily: "Segoe UI"

    /// Catalog for Settings UI (preview chips) — blueExtra first (default)
    readonly property var themes: [
        {
            id: "blueExtra",
            labelKey: "aegra.settings.theme.blue_extra",
            previewBg: "#1b2a40",
            previewCard: "#2a3f5c",
            previewAccent: "#4fc1ff",
            previewText: "#ffffff"
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
            colorMenuActive: "#e81123",
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
            colorUnallocatedText: "#9a9a9a"
        },
        "blueExtra": {
            colorBg: "#1e3048",
            colorCard: "#2a4260",
            colorBorder: "#4a6a90",
            colorHeader: "#243a56",
            colorSidebar: "#243a56",
            colorTableHeader: "#34506e",
            colorTableRow: "#2a4260",
            colorTableAlt: "#314a6a",
            colorPopup: "#2a4260",
            colorInput: "#243a56",
            colorListItem: "#34506e",
            colorListItemAlt: "#2a4260",
            colorAccentRed: "#ff5c5c",
            colorAccentBlue: "#4fc1ff",
            colorGreen: "#57d38c",
            colorMenuActive: "#4fc1ff",
            colorTextWhite: "#f0f4f8",
            colorTextGrey: "#b4c8dc",
            colorTextDim: "#8aa4c0",
            colorHover: "#3a5878",
            colorHoverClose: "#ff5c5c",
            colorButton: "#3a5574",
            colorButtonHover: "#466888",
            colorButtonDisabled: "#2d4560",
            colorButtonDisabledText: "#8aa4c0",
            colorProgressTrack: "#3a5574",
            colorCalendarMuted: "#8aa4c0",
            colorCalendarHasBackup: "#355878",
            colorToastSuccessBg: "#1e3d32",
            colorToastSuccessBorder: "#57d38c",
            colorToastErrorBg: "#3d2428",
            colorToastErrorBorder: "#ff5c5c",
            colorScrim: "#99000000",
            volumeColors: [
                "#4d6d8c", "#5a7a98", "#436384", "#6586a4",
                "#4a6a88", "#6e90ac", "#3f5e7e", "#7898b4"
            ],
            colorVolumeText: "#f0f4f8",
            colorUnallocated: "#252f3d",
            colorUnallocatedHatch: "#3d4a5c",
            colorUnallocatedText: "#9aabbc"
        },
        "light": {
            colorBg: "#f0f0f0",
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
            colorUnallocatedText: "#616161"
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
