import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import VulkanMedia
import "Format.js" as Format

/*! 252px right dock describing the focused item and the current selection. */
Rectangle {
    id: root

    property var fsModel: null
    property bool pinned: false
    //! Focused row, mirrored from the model.
    property string itemName: "—"
    property string itemPath: ""
    property string itemFormat: ""
    property string itemThumbnail: ""
    property real itemSize: 0
    property string itemDimensions: ""
    property var itemModified: undefined

    signal collapseRequested()
    signal pinToggled()
    signal openRequested()

    implicitWidth: 252
    color: Theme.panel

    Rectangle {
        width: 1
        height: parent.height
        color: Theme.borderSoft
        z: 2
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        PanelHeader {
            Layout.fillWidth: true
            title: "Details"

            IconButton {
                width: 20
                height: 20
                glyph: "⚲"
                glyphSize: 11
                checked: root.pinned
                onClicked: root.pinToggled()
            }
            IconButton {
                width: 20
                height: 20
                glyph: "⌃"
                glyphSize: 10
                onClicked: root.collapseRequested()
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: root.width
                spacing: 10

                // Selection banner.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    Layout.topMargin: 11
                    Layout.preferredHeight: 32
                    visible: root.fsModel && root.fsModel.selectionCount > 0
                    radius: Theme.radiusSm
                    color: Theme.tintFill
                    border.width: 1
                    border.color: Theme.tintRing

                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: 9
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 7

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14
                            height: 14
                            radius: 7
                            color: Theme.accent

                            Text {
                                anchors.centerIn: parent
                                text: "✓"
                                color: Theme.bg
                                font.family: Theme.uiFamily
                                font.pixelSize: 9
                            }
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.fsModel
                                  ? qsTr("%1 selected · %2").arg(root.fsModel.selectionCount)
                                        .arg(Format.bytes(root.fsModel.selectionBytes))
                                  : ""
                            color: Theme.text
                            font.family: Theme.uiFamily
                            font.pixelSize: 11
                        }
                    }
                }

                // Preview with format badge.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    Layout.preferredHeight: (root.width - 24) * 10 / 16
                    radius: Theme.radius
                    color: Theme.surface
                    border.width: 1
                    border.color: Theme.border
                    clip: true

                    Image {
                        anchors.fill: parent
                        anchors.margins: 1
                        source: root.itemThumbnail
                        asynchronous: true
                        fillMode: Image.PreserveAspectCrop
                        sourceSize.width: 320
                        sourceSize.height: 200
                        opacity: status === Image.Ready ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: Theme.durThumb } }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        anchors.margins: 7
                        width: formatLabel.implicitWidth + 10
                        height: 15
                        radius: 3
                        visible: root.itemFormat.length > 0
                        color: Qt.alpha(Theme.bg, 0.85)
                        border.width: 1
                        border.color: Theme.tintRing

                        Text {
                            id: formatLabel
                            anchors.centerIn: parent
                            text: root.itemFormat
                            color: Theme.accent300
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fsMicro
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    spacing: 2

                    Text {
                        Layout.fillWidth: true
                        text: root.itemName
                        color: Theme.text
                        font.family: Theme.uiFamily
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        elide: Text.ElideMiddle
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.itemPath
                        color: Theme.neutral600
                        font.family: Theme.monoFamily
                        font.pixelSize: 10
                        elide: Text.ElideMiddle
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    spacing: 6

                    KeyValueRow { label: qsTr("Size");        value: Format.bytes(root.itemSize) }
                    KeyValueRow { label: qsTr("Dimensions");  value: root.itemDimensions.length ? root.itemDimensions : "—" }
                    KeyValueRow { label: qsTr("Channels");    value: root.itemDimensions.length ? "RGBA · 16-bit" : "—" }
                    KeyValueRow { label: qsTr("Color space"); value: root.itemDimensions.length ? "Rec. 709" : "—" }
                    KeyValueRow { label: qsTr("Modified");    value: Format.modified(root.itemModified) }
                }

                // Tags.
                Flow {
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12
                    spacing: 5

                    Chip { text: "reference"; active: true }
                    Chip { text: "graded" }
                    Chip { text: "client-a" }
                    Chip { text: "+ add"; outlined: true }
                }

                Item { Layout.fillHeight: true; Layout.minimumHeight: 12 }
            }
        }

        // Pinned action bar.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 43
            color: Theme.panel

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.borderSoft
            }

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                // Primary action: outlined accent, never a solid fill.
                IconButton {
                    width: 96
                    height: 27
                    text: qsTr("Open")
                    labelSize: 11
                    outlined: true
                    glyph: ""
                    onClicked: root.openRequested()
                }

                IconButton {
                    width: 31
                    height: 27
                    glyph: "⤓"
                    glyphSize: 13
                    outlined: true
                }

                IconButton {
                    width: 31
                    height: 27
                    glyph: "⋯"
                    glyphSize: 13
                    outlined: true
                }
            }
        }
    }
}
