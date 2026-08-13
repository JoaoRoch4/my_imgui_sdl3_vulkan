import QtQuick
import QtQuick.Controls.Basic
import VulkanMedia

/*! Pill-shaped tag. Active chips use the tinted accent-900/800 pair; the
    "+ add" variant is outlined with a 1px accent border. */
AbstractButton {
    id: control

    property bool active: false
    property bool outlined: false

    implicitHeight: 20
    implicitWidth: label.implicitWidth + 18
    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    background: Rectangle {
        radius: height / 2
        color: control.outlined ? "transparent"
             : control.active ? Theme.tintFill
             : control.hovered ? Qt.lighter(Theme.surface, 1.15)
             : Theme.surface
        border.width: 1
        border.color: control.outlined ? Theme.accent
                    : control.active ? Theme.tintRing
                    : Theme.border

        Behavior on color { ColorAnimation { duration: Theme.durHover } }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: height / 2
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
            visible: control.visualFocus
        }
    }

    contentItem: Text {
        id: label
        text: control.text
        color: control.outlined ? Theme.accent300
             : control.active ? Theme.text
             : Theme.bodyText
        font.family: Theme.uiFamily
        font.pixelSize: Theme.fsSmall
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
