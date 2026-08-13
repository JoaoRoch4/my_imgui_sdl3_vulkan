import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import VulkanMedia

/*! Tool-window menu (Panels ▸ / Ctrl+Shift+P). Rows carry an accent-400
    checkmark when the panel is open; rows with placement choices open a 186px
    submenu. */
Popup {
    id: root

    property var openPanels: ["files", "details", "console"]

    signal panelToggled(string panel)
    signal panelPlacement(string panel, string placement)
    signal layoutReset()

    width: 306
    padding: 7
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property var groups: [
        { title: "Left dock", rows: [
            { id: "files",     glyph: "🗂", label: "File browser",  shortcut: "Alt+1", sub: true },
            { id: "places",    glyph: "▤",  label: "Places",        shortcut: "",      sub: false },
            { id: "launcher",  glyph: "▦",  label: "Panel launcher", shortcut: "",     sub: false } ] },
        { title: "Right dock", rows: [
            { id: "details",   glyph: "≡",  label: "Details",       shortcut: "Alt+2", sub: true },
            { id: "metadata",  glyph: "✎",  label: "Metadata editor", shortcut: "",    sub: true },
            { id: "histogram", glyph: "▥",  label: "Histogram",     shortcut: "",      sub: false } ] },
        { title: "Bottom dock", rows: [
            { id: "console",   glyph: "▤",  label: "Console",       shortcut: "F1",    sub: true },
            { id: "config",    glyph: "⚙",  label: "Runtime config", shortcut: "F2",   sub: true },
            { id: "downloads", glyph: "⤓",  label: "Downloads",     shortcut: "Alt+3", sub: true },
            { id: "threads",   glyph: "⑃",  label: "Thread reflection", shortcut: "Alt+4", sub: true } ] }
    ]

    readonly property var placements: [
        "Left dock", "Right dock", "Bottom dock", "Float", "New window", "As a tab"
    ]

    background: Rectangle {
        id: bg
        color: Theme.surface
        radius: 9
        border.width: 1
        border.color: Theme.neutral700

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowBlur: 0.9
            shadowVerticalOffset: 7
            shadowOpacity: 0.55
            shadowColor: Qt.rgba(0, 0, 0, 1)
        }
    }

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durPopup; easing.type: Easing.OutCubic }
            NumberAnimation { property: "y"; from: root.y - 6; to: root.y; duration: Theme.durPopup; easing.type: Easing.OutCubic }
        }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; to: 0; duration: 90 }
    }

    onClosed: submenu.close()

    contentItem: Column {
        spacing: 3

        // Header.
        Item {
            width: parent.width
            height: 22

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                text: "Panels"
                color: Theme.neutral500
                font.family: Theme.uiFamily
                font.pixelSize: Theme.fsMicro
                font.weight: Font.Medium
                font.capitalization: Font.AllUppercase
                font.letterSpacing: Theme.fsMicro * 0.09
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                text: "Ctrl+Shift+P"
                color: Theme.neutral700
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsMicro
            }
        }

        Repeater {
            model: root.groups

            delegate: Column {
                required property var modelData
                width: 292
                spacing: 1

                Item {
                    width: parent.width
                    height: 20

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 4
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 3
                        text: modelData.title
                        color: Theme.neutral500
                        font.family: Theme.uiFamily
                        font.pixelSize: Theme.fsMicro
                        font.weight: Font.Medium
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: Theme.fsMicro * 0.09
                    }
                }

                Repeater {
                    model: modelData.rows

                    delegate: Rectangle {
                        id: row
                        required property var modelData

                        readonly property bool isOpen: root.openPanels.indexOf(modelData.id) >= 0

                        width: 292
                        height: 25
                        radius: 5
                        color: rowHover.hovered ? Theme.tintFill : "transparent"
                        border.width: rowHover.hovered ? 1 : 0
                        border.color: Theme.tintRing
                        Behavior on color { ColorAnimation { duration: Theme.durHover } }

                        Row {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: row.modelData.glyph
                                color: row.isOpen ? Theme.accent400 : Theme.mutedText
                                font.family: Theme.uiFamily
                                font.pixelSize: 13
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: row.modelData.label
                                color: row.isOpen ? Theme.text : Theme.bodyText
                                font.family: Theme.uiFamily
                                font.pixelSize: 11
                            }
                        }

                        Row {
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: row.modelData.shortcut
                                color: Theme.neutral700
                                font.family: Theme.monoFamily
                                font.pixelSize: Theme.fsMicro
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "✓"
                                visible: row.isOpen
                                color: Theme.accent400
                                font.family: Theme.uiFamily
                                font.pixelSize: 11
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "›"
                                visible: row.modelData.sub === true
                                color: Theme.neutral600
                                font.family: Theme.uiFamily
                                font.pixelSize: 12
                            }
                        }

                        HoverHandler {
                            id: rowHover
                            onHoveredChanged: {
                                if (hovered && row.modelData.sub === true) {
                                    submenu.panelId = row.modelData.id
                                    const p = row.mapToItem(root.parent, row.width - 8, 0)
                                    submenu.x = p.x
                                    submenu.y = p.y
                                    submenu.open()
                                }
                            }
                        }

                        TapHandler {
                            onTapped: {
                                root.panelToggled(row.modelData.id)
                                root.close()
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            width: 292
            height: 1
            color: Theme.borderSoft
        }

        Rectangle {
            width: 292
            height: 25
            radius: 5
            color: footHover.hovered ? Theme.tintFill : "transparent"
            border.width: footHover.hovered ? 1 : 0
            border.color: Theme.tintRing

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Reset to default layout")
                color: Theme.bodyText
                font.family: Theme.uiFamily
                font.pixelSize: 11
            }

            HoverHandler { id: footHover }
            TapHandler {
                onTapped: {
                    root.layoutReset()
                    root.close()
                }
            }
        }
    }

    // ---- placement submenu ----------------------------------------------
    Popup {
        id: submenu

        property string panelId: ""

        parent: root.parent
        width: 186
        padding: 6
        closePolicy: Popup.NoAutoClose
        visible: false

        background: Rectangle {
            color: Theme.surface
            radius: 9
            border.width: 1
            border.color: Theme.neutral700

            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowBlur: 0.9
                shadowVerticalOffset: 7
                shadowOpacity: 0.55
                shadowColor: Qt.rgba(0, 0, 0, 1)
            }
        }

        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durPopup; easing.type: Easing.OutCubic }
        }

        contentItem: Column {
            spacing: 1

            Text {
                text: qsTr("Open in")
                color: Theme.neutral500
                font.family: Theme.uiFamily
                font.pixelSize: Theme.fsMicro
                font.weight: Font.Medium
                font.capitalization: Font.AllUppercase
                font.letterSpacing: Theme.fsMicro * 0.09
                bottomPadding: 4
                leftPadding: 6
            }

            Repeater {
                model: root.placements

                delegate: Rectangle {
                    required property string modelData

                    width: 174
                    height: 24
                    radius: 5
                    color: placeHover.hovered ? Theme.tintFill : "transparent"
                    border.width: placeHover.hovered ? 1 : 0
                    border.color: Theme.tintRing

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        color: Theme.bodyText
                        font.family: Theme.uiFamily
                        font.pixelSize: 11
                    }

                    HoverHandler { id: placeHover }
                    TapHandler {
                        onTapped: {
                            root.panelPlacement(submenu.panelId, modelData)
                            submenu.close()
                            root.close()
                        }
                    }
                }
            }
        }
    }
}
