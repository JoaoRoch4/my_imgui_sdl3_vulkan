import QtQuick
import QtQuick.Controls.Basic
import VulkanMedia

/*! Scrollable places / volumes list. The current folder gets the tinted row
    plus a 2px accent left marker; volumes distinguish mounted, unmounted and
    connected-network states. */
Item {
    id: root

    property string currentFolder: "reference"

    signal placeActivated(string path)
    signal ejectRequested(string name)
    signal mountRequested(string name)

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: Theme.borderSoft
    }

    ListModel {
        id: places

        ListElement { section: "Places"; glyph: "🏠"; label: "Home";      path: "/home";                 kind: "place" }
        ListElement { section: "Places"; glyph: "▤";  label: "Projects";  path: "/media/projects";       kind: "place" }
        ListElement { section: "Places"; glyph: "▦";  label: "reference"; path: "/media/reference";      kind: "place" }
        ListElement { section: "Places"; glyph: "⤓";  label: "Downloads"; path: "/home/downloads";       kind: "place" }
        ListElement { section: "Places"; glyph: "★";  label: "Favorites"; path: "/home/favorites";       kind: "place" }
        ListElement { section: "Volumes"; glyph: "▮"; label: "System";    path: "/";                     kind: "volume";  mounted: true;  network: false; connected: false }
        ListElement { section: "Volumes"; glyph: "▮"; label: "Scratch 2TB"; path: "/mnt/scratch";        kind: "volume";  mounted: true;  network: false; connected: false }
        ListElement { section: "Volumes"; glyph: "▮"; label: "Archive HDD"; path: "/mnt/archive";        kind: "volume";  mounted: false; network: false; connected: false }
        ListElement { section: "Volumes"; glyph: "☁"; label: "nas-media";  path: "/net/nas-media";       kind: "volume";  mounted: true;  network: true;  connected: true }
        ListElement { section: "Volumes"; glyph: "☁"; label: "render-farm"; path: "/net/render-farm";    kind: "volume";  mounted: false; network: true;  connected: false }
    }

    ListView {
        id: view
        anchors.fill: parent
        anchors.topMargin: 1
        clip: true
        model: places
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
            contentItem: Rectangle {
                implicitWidth: 3
                radius: 1.5
                color: Theme.neutral700
                opacity: parent.pressed ? 0.9 : 0.5
            }
        }

        section.property: "section"
        section.delegate: Item {
            required property string section
            width: view.width
            height: 24

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 4
                text: section
                color: Theme.neutral500
                font.family: Theme.uiFamily
                font.pixelSize: Theme.fsMicro
                font.weight: Font.Medium
                font.capitalization: Font.AllUppercase
                font.letterSpacing: Theme.fsMicro * 0.09
            }
        }

        delegate: Item {
            id: row
            required property int index
            required property string glyph
            required property string label
            required property string path
            required property string kind
            required property bool mounted
            required property bool network
            required property bool connected

            readonly property bool isCurrent: label === root.currentFolder
            readonly property bool dimmed: kind === "volume" && !mounted

            width: view.width
            height: 22

            Rectangle {
                anchors.fill: parent
                color: row.isCurrent ? Theme.tintFill
                     : rowHover.hovered ? Qt.alpha(Theme.accent, 0.08)
                     : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.durHover } }
            }

            // 2px accent marker for the folder currently shown in the grid.
            Rectangle {
                width: 2
                height: parent.height
                color: Theme.accent
                visible: row.isCurrent
            }

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 7

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.glyph
                    font.family: Theme.uiFamily
                    font.pixelSize: 13
                    color: row.dimmed ? Theme.neutral700
                         : (row.kind === "volume" && row.mounted) || row.isCurrent ? Theme.accent400
                         : Theme.mutedText
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.label
                    font.family: Theme.uiFamily
                    font.pixelSize: Theme.fsBody
                    color: row.dimmed ? Theme.neutral600 : (row.isCurrent ? Theme.text : Theme.bodyText)
                    opacity: row.dimmed ? 0.7 : 1
                }

                // Connected network share.
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.network && row.connected
                    width: 5
                    height: 5
                    radius: 2.5
                    color: Theme.accent
                }
            }

            // Trailing action: eject when mounted, mount when not.
            IconButton {
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                width: 18
                height: 18
                glyph: "⏏"
                glyphSize: 10
                visible: row.kind === "volume" && row.mounted && (rowHover.hovered || row.isCurrent)
                onClicked: root.ejectRequested(row.label)
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                visible: row.kind === "volume" && !row.mounted && rowHover.hovered
                text: qsTr("mount")
                color: Theme.accent300
                font.family: Theme.uiFamily
                font.pixelSize: 10
                TapHandler { onTapped: root.mountRequested(row.label) }
            }

            HoverHandler { id: rowHover }
            TapHandler { onTapped: root.placeActivated(row.path) }
        }
    }
}
