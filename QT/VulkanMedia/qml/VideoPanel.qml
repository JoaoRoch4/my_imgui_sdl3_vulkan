import QtQuick
import VulkanMedia

/*! Placeholder video surface. mpv/libplacebo integration is out of scope for
    this scaffold — only the OSD chrome is here, drawn over an empty surface. */
Rectangle {
    id: root

    property string title: "vfx_comp_v07.mov"
    property real position: 0.34
    property string elapsed: "0:03"
    property string total: "0:09"
    property bool playing: false

    color: Theme.bg

    Rectangle {
        anchors.fill: parent
        anchors.margins: 14
        radius: Theme.radius
        color: Theme.surface
        border.width: 1
        border.color: Theme.border

        Column {
            anchors.centerIn: parent
            spacing: 7

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "▶"
                color: Theme.neutral700
                font.family: Theme.uiFamily
                font.pixelSize: 34
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Video surface placeholder")
                color: Theme.mutedText
                font.family: Theme.uiFamily
                font.pixelSize: 12
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("mpv · libplacebo not wired in this scaffold")
                color: Theme.neutral700
                font.family: Theme.monoFamily
                font.pixelSize: 10
            }
        }

        // OSD chrome.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 1
            height: 46
            color: Qt.alpha(Theme.bg, 0.85)
            radius: Theme.radius

            Row {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 10

                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: root.playing ? "⏸" : "▶"
                    glyphSize: 13
                    onClicked: root.playing = !root.playing
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.elapsed
                    color: Theme.bodyText
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsSmall
                }

                // Seek bar — accent as a line, never a large fill.
                Item {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 260
                    height: 16

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: 2
                        radius: 1
                        color: Theme.border
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width * root.position
                        height: 2
                        radius: 1
                        color: Theme.accent
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        x: parent.width * root.position - 4
                        width: 8
                        height: 8
                        radius: 4
                        color: Theme.accent400
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.total
                    color: Theme.neutral600
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsSmall
                }

                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: "♪"
                    glyphSize: 13
                }

                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: "⛶"
                    glyphSize: 13
                }
            }
        }

        // Title badge.
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 10
            width: titleLabel.implicitWidth + 16
            height: 22
            radius: Theme.radiusSm
            color: Qt.alpha(Theme.bg, 0.85)
            border.width: 1
            border.color: Theme.border

            Text {
                id: titleLabel
                anchors.centerIn: parent
                text: root.title
                color: Theme.bodyText
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsSmall
            }
        }
    }
}
