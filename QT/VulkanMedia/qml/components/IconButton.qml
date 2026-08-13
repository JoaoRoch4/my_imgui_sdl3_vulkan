import QtQuick
import QtQuick.Controls.Basic
import VulkanMedia

/*!
 * Square-ish chrome button carrying a glyph and (optionally) a label.
 * Idle is transparent; hover fades in an accent tint; the checked state uses
 * the accent-900 fill + accent-800 ring the design system reserves for tinted
 * surfaces. The accent is never used as a large solid fill.
 */
AbstractButton {
    id: control

    property string glyph: ""
    property real glyphSize: 15
    property real labelSize: 11
    property color glyphColor: control.checked ? Theme.accent400
                                               : (control.enabled ? Theme.mutedText : Theme.neutral700)
    property bool showLabel: text.length > 0
    //! Outlined variant: 1px accent border, transparent background.
    property bool outlined: false
    //! Resting fill; launcher tiles sit on `surface`, chrome buttons on nothing.
    property color baseColor: "transparent"
    //! 1px neutral-800 inset border in the resting state.
    property bool inset: false
    property real radius: Theme.radiusSm
    //! Gap between glyph and label. AbstractButton::spacing is FINAL, so this
    //! carries its own name.
    property real contentSpacing: 4

    implicitWidth: 26
    implicitHeight: 26
    hoverEnabled: enabled
    opacity: enabled ? 1 : 0.45
    focusPolicy: Qt.TabFocus

    background: Rectangle {
        radius: control.radius
        color: control.checked ? Theme.tintFill
             : control.down ? Qt.alpha(Theme.accent400, 0.22)
             : control.hovered ? Qt.alpha(Theme.accent, 0.12)
             : control.baseColor
        border.width: control.checked || control.outlined || control.inset ? 1 : 0
        border.color: control.checked ? Theme.tintRing
                    : control.outlined ? Theme.accent
                    : Theme.border

        Behavior on color {
            ColorAnimation { duration: Theme.durHover; easing.type: Easing.OutQuad }
        }

        // 2px accent focus ring, 2px offset — never the platform default.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: parent.radius + 2
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
            visible: control.visualFocus
        }
    }

    contentItem: Column {
        spacing: control.contentSpacing

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: control.glyph
            visible: control.glyph.length > 0
            color: control.glyphColor
            font.family: Theme.uiFamily
            font.pixelSize: control.glyphSize
            Behavior on color { ColorAnimation { duration: Theme.durHover } }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: control.text
            visible: control.showLabel
            color: control.checked ? Theme.text : Theme.bodyText
            font.family: Theme.uiFamily
            font.pixelSize: control.labelSize
            Behavior on color { ColorAnimation { duration: Theme.durHover } }
        }
    }
}
