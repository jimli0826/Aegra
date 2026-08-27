pragma ComponentBehavior: Bound

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."

// Modal Open Source Licenses dialog matching Figure 2.
Popup {
    id: root
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    anchors.centerIn: Overlay.overlay
    width: Math.min(680, parent ? parent.width - 32 : 680)
    height: Math.min(560, parent ? parent.height - 40 : 560)
    padding: 20

    background: Rectangle {
        radius: Theme.radiusCard
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.0; color: Theme.colorCard }
            GradientStop { position: 1.0; color: Theme.colorCardEnd }
        }
        border.width: 1
        border.color: Theme.colorBorder
    }

    readonly property var licensesData: [
        {
            name: "Qt",
            version: (typeof localeController !== "undefined" && localeController) ? localeController.qtVersion : "6.8.3",
            url: "https://www.qt.io",
            license: "GNU Lesser General Public License (LGPL) version 3",
            copyright: "Copyright (C) The Qt Company Ltd. and other contributors.",
            text: "This program is linked against Qt libraries under the GNU Lesser General Public License (LGPL) version 3.\nSource copy and licensing information: https://www.qt.io/licensing\nModifications: No modifications to Qt source code."
        },
        {
            name: "Dokan",
            version: "2.3.1",
            url: "https://github.com/dokan-dev/dokany",
            license: "GNU Lesser General Public License (LGPL) version 3 or later",
            copyright: "Copyright (C) 2007-2025 Dokan contributors",
            text: "Dokan is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser General Public License as published by the Free Software Foundation; either version 3 of the License, or (at your option) any later version."
        },
        {
            name: "Qt Core third-party components",
            version: "Qt 6.8.3 SBOM",
            url: "https://doc.qt.io/qt-6/licenses-used-in-qt.html#qt-core",
            license: "Apache-2.0, BSD-2/3-Clause, CC0-1.0, MIT, Unicode-3.0, Zlib, Public Domain and PCRE2 licenses",
            copyright: "See the Qt 6.8.3 SPDX SBOM and attribution documents for individual copyright holders.",
            text: "Bundled components: Apache Tika MIME definitions; BLAKE2; zlib 1.3.1; Robert Penner easing equations; double-conversion 3.3.0; MD4; MD5; PCRE2 and SLJIT 10.45; SHA-1; SHA-3 Keccak 3.2 and brg_endian; RFC 6234 SHA-384/SHA-512; SipHash; TinyCBOR 0.6.1; Unicode Character Database 34; and Unicode CLDR 46.1."
        },
        {
            name: "Qt GUI third-party components",
            version: "Qt 6.8.3 SBOM",
            url: "https://doc.qt.io/qt-6/licenses-used-in-qt.html#qt-gui",
            license: "BSD, FreeType, GPL-2.0, IJG, Libpng, MIT, Zlib and related permissive licenses",
            copyright: "See the Qt 6.8.3 SPDX SBOM and attribution documents for individual copyright holders.",
            text: "Bundled components: libpng 1.6.47; libjpeg-turbo 3.1.0; FreeType 2.13.3 and its BDF, PCF and zlib portions; HarfBuzz 10.4.0; Adobe Glyph List 1.7; D3D12 Memory Allocator; D3D12 mipmap generator; OpenGL and OpenGL ES headers; FreeType gray rasterizer; smooth-scaling algorithm; Vulkan Memory Allocator 3.0.1 and Vulkan registry 1.3.223; WebGradients; sRGB ICC profile; MD4C 0.5.2; and zlib 1.3.1."
        },
        {
            name: "Qt Network third-party components",
            version: "Qt 6.8.3 SBOM",
            url: "https://doc.qt.io/qt-6/licenses-used-in-qt.html#qt-network",
            license: "BSD 3-Clause, Mozilla Public License 2.0 and Zlib License",
            copyright: "See the Qt 6.8.3 SPDX SBOM and attribution documents for individual copyright holders.",
            text: "Bundled components: zlib 1.3.1; the Public Suffix List snapshot fetched on 2025-01-22; and libpsl. Windows TLS is provided by Qt's Schannel backend unless a separately distributed OpenSSL runtime is added to the product package."
        },
        {
            name: "Qt QML and Quick Controls third-party components",
            version: "Qt 6.8.3 SBOM",
            url: "https://doc.qt.io/qt-6/licenses-used-in-qt.html#qt-qml",
            license: "BSD 2-Clause and MIT License",
            copyright: "See the Qt 6.8.3 SPDX SBOM and attribution documents for individual copyright holders.",
            text: "Bundled components used by the linked Qt modules: JavaScriptCore Macro Assembler (BSD 2-Clause) and Angular Material shadow values used by Qt Quick Controls (MIT)."
        },
        {
            name: "Zstandard (zstd)",
            version: "1.5.7",
            url: "https://github.com/facebook/zstd",
            license: "BSD 3-Clause License",
            copyright: "Copyright (c) 2016-present, Facebook, Inc. / Meta Platforms, Inc. All rights reserved.",
            text: "Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:\n\n* Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.\n* Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.\n* Neither the name Facebook nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission."
        },
        {
            name: "libsodium",
            version: "1.0.22",
            url: "https://github.com/jedisct1/libsodium",
            license: "ISC License",
            copyright: "Copyright (c) 2013-2026 Frank Denis <j at pureftpd dot org>",
            text: "Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies.\n\nTHE SOFTWARE IS PROVIDED \"AS IS\" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS."
        },
        {
            name: "SQLite",
            version: "3.53.4",
            url: "https://www.sqlite.org",
            license: "Public Domain (SQLite Blessing)",
            copyright: "2001 September 15 - The author disclaims copyright to this source code.",
            text: "May you do good and not evil.\nMay you find forgiveness for yourself and forgive others.\nMay you share freely, never taking more than you give."
        },
        {
            name: "JSON for Modern C++ (nlohmann/json)",
            version: "3.12.0",
            url: "https://github.com/nlohmann/json",
            license: "MIT License",
            copyright: "Copyright (c) 2013-2025 Niels Lohmann",
            text: "Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the \"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software."
        },
        {
            name: "spdlog",
            version: "1.15.3",
            url: "https://github.com/gabime/spdlog",
            license: "MIT License",
            copyright: "Copyright (c) 2015-present, Gabi Melman and spdlog contributors",
            text: "Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the \"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software."
        },
        {
            name: "{fmt}",
            version: "11.2.0",
            url: "https://github.com/fmtlib/fmt",
            license: "MIT License",
            copyright: "Copyright (c) 2012-present, Victor Zverovich and fmt contributors",
            text: "spdlog includes and uses a bundled header-only copy of {fmt}. Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the \"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software."
        },
        {
            name: "Mini Shell extension Framework (MSF)",
            version: "0.93",
            url: "https://github.com/vbaderks/msf",
            license: "GNU Lesser General Public License (LGPL) version 3",
            copyright: "Copyright (C) Victor Derks",
            text: "Aegra uses vendored MSF C++ template headers to implement its Windows Explorer shell extension. MSF is distributed under the GNU Lesser General Public License version 3."
        }
    ]

    contentItem: ColumnLayout {
        width: root.availableWidth
        height: root.availableHeight
        spacing: 12

        // Header
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Text {
                    text: qsTrId("aegra.licenses.title")
                    color: Theme.colorTextWhite
                    font.pixelSize: 15
                    font.bold: true
                    font.family: Theme.fontFamily
                }

                Text {
                    text: qsTrId("aegra.licenses.subtitle")
                    color: Theme.colorTextGrey
                    font.pixelSize: 12
                    font.family: Theme.fontFamily
                }
            }

            AppButton {
                text: qsTrId("aegra.licenses.about_qt")
                implicitHeight: 26
                leftPadding: 10
                rightPadding: 10
                onClicked: qtAboutPopup.open()
            }
        }

        // Scrollable license cards
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ListView {
                id: licenseListView
                width: parent.width
                spacing: 12
                model: root.licensesData

                delegate: Rectangle {
                    id: itemCard
                    required property var modelData

                    width: licenseListView.width - 8
                    radius: Theme.radiusControl
                    color: Theme.colorInput
                    border.width: 1
                    border.color: Theme.colorBorder
                    implicitHeight: cardCol.implicitHeight + 20

                    ColumnLayout {
                        id: cardCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 12
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Text {
                                text: itemCard.modelData.name
                                color: Theme.colorTextWhite
                                font.pixelSize: 14
                                font.bold: true
                                font.family: Theme.fontFamily
                            }

                            Text {
                                text: "v" + itemCard.modelData.version
                                color: Theme.colorTextDim
                                font.pixelSize: 11
                                font.family: Theme.fontFamily
                            }

                            Item { Layout.fillWidth: true }

                            LinkButton {
                                text: itemCard.modelData.url
                                font.pixelSize: 11
                                onClicked: Qt.openUrlExternally(itemCard.modelData.url)
                            }
                        }

                        Text {
                            text: qsTrId("aegra.licenses.licensed_under") + " " + itemCard.modelData.license
                            color: Theme.colorAccentBlue
                            font.pixelSize: 12
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Text {
                            text: itemCard.modelData.copyright
                            color: Theme.colorTextGrey
                            font.pixelSize: 11
                            font.family: Theme.fontFamily
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            radius: 4
                            color: Theme.colorPopup
                            border.width: 1
                            border.color: Theme.colorBorder
                            implicitHeight: licenseText.implicitHeight + 12

                            Text {
                                id: licenseText
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 6
                                text: itemCard.modelData.text
                                color: Theme.colorTextGrey
                                font.pixelSize: 11
                                font.family: "Consolas, monospace"
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }
            }
        }

        // Footer
        RowLayout {
            Layout.fillWidth: true

            Item { Layout.fillWidth: true }

            AppButton {
                text: qsTrId("aegra.common.ok")
                primary: true
                implicitHeight: 30
                leftPadding: 24
                rightPadding: 24
                onClicked: root.close()
            }
        }
    }

    // Popup for "About Qt"
    Popup {
        id: qtAboutPopup
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        anchors.centerIn: Overlay.overlay
        width: Math.min(480, parent ? parent.width - 40 : 480)
        padding: 18

        background: Rectangle {
            radius: Theme.radiusControl
            color: Theme.colorCard
            border.width: 1
            border.color: Theme.colorBorder
        }

        contentItem: ColumnLayout {
            spacing: 12

            RowLayout {
                Layout.fillWidth: true

                Text {
                    text: qsTrId("aegra.licenses.about_qt_title")
                    color: Theme.colorTextWhite
                    font.pixelSize: 15
                    font.bold: true
                    font.family: Theme.fontFamily
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    width: 24
                    height: 24
                    radius: 4
                    color: qtCloseMouse.containsMouse ? Theme.colorHover : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "\u2715"
                        color: qtCloseMouse.containsMouse ? Theme.colorHoverClose : Theme.colorTextGrey
                        font.pixelSize: 12
                        font.family: Theme.fontFamily
                    }

                    MouseArea {
                        id: qtCloseMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: qtAboutPopup.close()
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTrId("aegra.licenses.about_qt_desc")
                      .arg((typeof localeController !== "undefined" && localeController) ? localeController.qtVersion : "6.8.3")
                color: Theme.colorTextGrey
                font.pixelSize: 12
                font.family: Theme.fontFamily
                wrapMode: Text.WordWrap
            }

            LinkButton {
                text: "https://www.qt.io"
                onClicked: Qt.openUrlExternally("https://www.qt.io")
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                AppButton {
                    text: qsTrId("aegra.common.ok")
                    primary: true
                    implicitHeight: 28
                    leftPadding: 16
                    rightPadding: 16
                    onClicked: qtAboutPopup.close()
                }
            }
        }
    }
}
