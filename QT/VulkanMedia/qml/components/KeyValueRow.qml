import QtQuick
import QtQuick.Layouts
import VulkanMedia

/*! Details-panel metadata row: fixed 80px label column, monospace value. */
RowLayout {
    id: root

    property string label: ""
    property string value: ""

    spacing: 8
    Layout.fillWidth: true

    Text {
        Layout.preferredWidth: 80
        text: root.label
        color: Theme.neutral600
        font.family: Theme.uiFamily
        font.pixelSize: Theme.fsSmall
        elide: Text.ElideRight
    }

    Text {
        Layout.fillWidth: true
        text: root.value
        color: Theme.bodyText
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fsSmall
        elide: Text.ElideRight
    }
}
