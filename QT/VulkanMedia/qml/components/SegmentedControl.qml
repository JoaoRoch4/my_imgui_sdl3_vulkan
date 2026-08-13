import QtQuick
import VulkanMedia

/*! View-mode switch: 26px outer shell, 24×22 options, active option carries the
    accent-900 fill and an accent-400 glyph. */
Item {
    id: root

    //! [{ glyph, name }, …]
    property var options: []
    property int currentIndex: 0

    signal activated(int index)

    implicitHeight: 26
    implicitWidth: row.implicitWidth + 4

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusSm
        color: Theme.bg
        border.width: 1
        border.color: Theme.border
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 0

        Repeater {
            model: root.options

            delegate: Item {
                required property int index
                required property var modelData

                width: 24
                height: 22

                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: index === root.currentIndex ? Theme.tintFill
                         : hover.hovered ? Qt.alpha(Theme.accent, 0.10)
                         : "transparent"
                    border.width: index === root.currentIndex ? 1 : 0
                    border.color: Theme.tintRing
                    Behavior on color { ColorAnimation { duration: Theme.durHover } }
                }

                Text {
                    anchors.centerIn: parent
                    text: modelData.glyph
                    color: index === root.currentIndex ? Theme.accent400 : Theme.mutedText
                    font.family: Theme.uiFamily
                    font.pixelSize: 12
                    Behavior on color { ColorAnimation { duration: Theme.durHover } }
                }

                HoverHandler { id: hover }

                TapHandler {
                    onTapped: {
                        root.currentIndex = index
                        root.activated(index)
                    }
                }
            }
        }
    }
}
