import QtQuick
import VulkanMedia

/*! Empty-but-themed surface for the panels this scaffold does not implement
    yet (images, downloads, console, config, metadata, threads). */
Rectangle {
    id: root

    property string title: ""
    property string glyph: "▦"
    property string note: qsTr("Not implemented in this scaffold")

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
                text: root.glyph
                color: Theme.neutral700
                font.family: Theme.uiFamily
                font.pixelSize: 30
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.title
                color: Theme.text
                font.family: Theme.uiFamily
                font.pixelSize: 13
                font.weight: Font.Medium
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.note
                color: Theme.neutral600
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsSmall
            }
        }
    }
}
