import QtQuick
import VulkanMedia
import "Format.js" as Format

/*! 26px footer: selection summary on the left, runtime counters on the right. */
Rectangle {
    id: root

    property int selectedCount: 0
    property int totalCount: 0
    property real selectionBytes: 0
    property int thumbsReady: 0
    property int thumbsTotal: 0
    property int downloadCount: 0
    property string renderer: "Vulkan · RTX 4070"
    //! Live frame rate. Turning this off stops the continuous repaint.
    property bool showFps: true

    implicitHeight: 26
    color: Theme.chrome

    component Cell: Text {
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        color: Theme.neutral600
        font.family: Theme.monoFamily
        font.pixelSize: 11
    }

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.borderSoft
    }

    FrameAnimation {
        id: frames
        running: root.showFps && root.visible
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: root.selectedCount > 0
              ? qsTr("%1 of %2 selected · %3").arg(root.selectedCount).arg(root.totalCount)
                    .arg(Format.bytes(root.selectionBytes))
              : qsTr("%1 items").arg(root.totalCount)
        color: Theme.neutral600
        font.family: Theme.monoFamily
        font.pixelSize: 11
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 14

        Cell {
            text: qsTr("thumbs %1/%2").arg(root.thumbsReady).arg(root.thumbsTotal)
            color: root.thumbsTotal > 0 && root.thumbsReady < root.thumbsTotal
                   ? Theme.accent400 : Theme.neutral600
        }
        Cell {
            text: qsTr("%1 downloads").arg(root.downloadCount)
            visible: root.downloadCount > 0
        }
        Cell { text: root.renderer }
        Cell {
            text: root.showFps
                  ? qsTr("%1 fps").arg(frames.smoothFrameTime > 0
                                       ? Math.round(1 / frames.smoothFrameTime) : 0)
                  : qsTr("— fps")
        }
    }
}
