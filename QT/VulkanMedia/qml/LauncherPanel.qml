import QtQuick
import QtQuick.Layouts
import VulkanMedia

/*! 176px left dock: the eight panel launchers over a scrollable places list. */
Rectangle {
    id: root

    property string activePanel: "files"
    property int downloadCount: 2
    property string currentFolder: "reference"

    signal panelActivated(string panel)
    signal collapseRequested()
    signal placeActivated(string path)

    implicitWidth: 176
    color: Theme.panel

    readonly property var launchers: [
        { id: "files",     glyph: "🗂", label: "Files" },
        { id: "images",    glyph: "🖼", label: "Images" },
        { id: "video",     glyph: "▶",  label: "Video" },
        { id: "downloads", glyph: "⤓",  label: "Downloads" },
        { id: "console",   glyph: "▤",  label: "Console" },
        { id: "config",    glyph: "⚙",  label: "Config" },
        { id: "metadata",  glyph: "✎",  label: "Metadata" },
        { id: "threads",   glyph: "⑃",  label: "Threads" }
    ]

    Rectangle {
        anchors.right: parent.right
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
            title: "Panels"

            IconButton {
                glyph: "⌃"
                width: 20
                height: 20
                glyphSize: 10
                onClicked: root.collapseRequested()
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 6
            rowSpacing: 6
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            Layout.topMargin: 9
            Layout.bottomMargin: 12

            Repeater {
                model: root.launchers

                delegate: IconButton {
                    required property var modelData

                    Layout.fillWidth: true
                    // Neutral preferred width, so the two columns split evenly
                    // instead of following each label's implicit width.
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 50
                    glyph: modelData.glyph
                    text: modelData.label
                    glyphSize: 17
                    labelSize: Theme.fsSmall
                    contentSpacing: 5
                    radius: Theme.radius
                    baseColor: Theme.surface
                    inset: true
                    checked: root.activePanel === modelData.id
                    onClicked: root.panelActivated(modelData.id)

                    // Downloads badge — accent-400 mono, top-right.
                    Text {
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 5
                        visible: modelData.id === "downloads" && root.downloadCount > 0
                        text: root.downloadCount
                        color: Theme.accent400
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsMicro
                    }
                }
            }
        }

        PlacesList {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentFolder: root.currentFolder
            onPlaceActivated: (path) => root.placeActivated(path)
        }
    }
}
