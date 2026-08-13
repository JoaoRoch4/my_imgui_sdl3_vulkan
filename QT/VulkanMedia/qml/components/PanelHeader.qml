import QtQuick
import VulkanMedia

/*! 29px dock header: uppercase micro-label on the left, caller-supplied
    actions on the right. */
Item {
    id: root

    property string title: ""
    //! Actions are appended right-to-left into the trailing row.
    default property alias actions: actionRow.data

    implicitHeight: 29

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: root.title
        color: Theme.neutral500
        font.family: Theme.uiFamily
        font.pixelSize: Theme.fsMicro
        font.weight: Font.Medium
        font.capitalization: Font.AllUppercase
        font.letterSpacing: Theme.fsMicro * 0.09
    }

    Row {
        id: actionRow
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2
    }
}
