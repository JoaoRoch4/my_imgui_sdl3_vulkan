import QtQuick
import QtQuick.Controls.Basic
import VulkanMedia
import "Format.js" as Format

/*! Thumbnail grid with rubber-band, ctrl/shift multi-select and a right-click
    context menu. All pointer handling lives in one overlay so selection,
    hover and banding stay consistent. */
Item {
    id: root

    property var fsModel: null
    property string folderName: "reference"
    property string filter: ""

    signal itemActivated(int index)
    signal contextAction(string action)

    readonly property int cellW: 172
    readonly property int cellH: 136
    property int hoveredIndex: -1

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Column {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.topMargin: 16
        spacing: 2

        Text {
            text: root.folderName
            color: Theme.text
            font.family: Theme.uiFamily
            font.pixelSize: 14
            font.weight: Font.Medium
        }

        Text {
            text: {
                const total = root.fsModel ? root.fsModel.count : 0
                const sel = root.fsModel ? root.fsModel.selectionCount : 0
                return sel > 0 ? qsTr("%1 items · %2 selected").arg(total).arg(sel)
                               : qsTr("%1 items").arg(total)
            }
            color: Theme.neutral600
            font.family: Theme.uiFamily
            font.pixelSize: Theme.fsBody
        }
    }

    GridView {
        id: grid

        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.topMargin: 16 + 40
        anchors.bottomMargin: 16
        clip: true
        cellWidth: root.cellW
        cellHeight: root.cellH
        model: root.fsModel
        boundsBehavior: Flickable.StopAtBounds
        interactive: false // the overlay owns dragging; wheel is forwarded

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
            contentItem: Rectangle {
                implicitWidth: 3
                radius: 1.5
                color: Theme.neutral700
                opacity: parent.pressed ? 0.9 : 0.5
            }
        }

        // Thumbnails fade + scale in as the decoder delivers them.
        add: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durThumb; easing.type: Easing.OutCubic }
                NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: Theme.durThumb; easing.type: Easing.OutCubic }
            }
        }
        displaced: Transition {
            NumberAnimation { properties: "x,y"; duration: Theme.durDisplace; easing.type: Easing.OutCubic }
        }

        delegate: Item {
            id: cell

            required property int index
            required property string name
            required property string kind
            required property string thumbnail
            required property string duration
            required property bool selected

            width: root.cellW - 12
            height: root.cellH - 12

            readonly property bool hovered: root.hoveredIndex === cell.index
            //! Non-matching cells dim rather than disappear, so positions hold.
            readonly property bool matchesFilter: root.filter.length === 0
                || cell.name.toLowerCase().indexOf(root.filter.toLowerCase()) >= 0

            opacity: matchesFilter ? 1 : 0.28
            Behavior on opacity { NumberAnimation { duration: Theme.durTab; easing.type: Easing.OutQuad } }

            Rectangle {
                id: frame
                width: parent.width
                height: parent.width * 10 / 16
                radius: Theme.radius
                color: cell.selected ? Theme.tintFill : Theme.surface
                border.width: cell.selected ? 2 : 1
                border.color: cell.selected ? Theme.accent
                            : cell.hovered ? Theme.neutral700
                            : Theme.border
                clip: true

                Behavior on color { ColorAnimation { duration: Theme.durHover } }
                Behavior on border.color { ColorAnimation { duration: Theme.durHover } }

                Image {
                    anchors.fill: parent
                    anchors.margins: cell.selected ? 2 : 1
                    source: cell.thumbnail
                    asynchronous: true
                    cache: true
                    fillMode: Image.PreserveAspectCrop
                    sourceSize.width: 320
                    sourceSize.height: 200
                    opacity: status === Image.Ready ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: Theme.durThumb; easing.type: Easing.OutCubic } }

                    // Loading spinner.
                    Item {
                        anchors.centerIn: parent
                        width: 18
                        height: 18
                        visible: parent.status !== Image.Ready

                        Rectangle {
                            id: spinner
                            anchors.fill: parent
                            radius: width / 2
                            color: "transparent"
                            border.width: 2
                            border.color: Qt.alpha(Theme.accent, 0.25)

                            Rectangle {
                                width: 4
                                height: 4
                                radius: 2
                                color: Theme.accent400
                                x: parent.width / 2 - 2
                                y: -2
                            }

                            RotationAnimator on rotation {
                                running: spinner.visible
                                from: 0
                                to: 360
                                duration: 900
                                loops: Animation.Infinite
                            }
                        }
                    }
                }

                // Selection check-circle.
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 6
                    width: 15
                    height: 15
                    radius: 7.5
                    visible: cell.selected
                    color: Theme.accent

                    Text {
                        anchors.centerIn: parent
                        text: "✓"
                        color: Theme.bg
                        font.family: Theme.uiFamily
                        font.pixelSize: 9
                    }
                }

                // Kind glyph for the tinted/selected state.
                Text {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    visible: cell.selected || cell.kind === "folder"
                    text: cell.kind === "folder" ? "🗂" : cell.kind === "video" ? "▶" : "▦"
                    color: cell.selected ? Theme.accent400 : Theme.mutedText
                    font.family: Theme.uiFamily
                    font.pixelSize: 11
                }

                // Duration badge for video.
                Rectangle {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 6
                    visible: cell.kind === "video" && cell.duration.length > 0
                    width: durationLabel.implicitWidth + 8
                    height: 14
                    radius: 3
                    color: Qt.alpha(Theme.bg, 0.85)

                    Text {
                        id: durationLabel
                        anchors.centerIn: parent
                        text: cell.duration
                        color: Theme.bodyText
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fsMicro
                    }
                }
            }

            Text {
                anchors.top: frame.bottom
                anchors.topMargin: 6
                width: parent.width
                text: cell.name
                color: cell.selected ? Theme.text : Theme.neutral400
                font.family: Theme.uiFamily
                font.pixelSize: 11
                elide: Text.ElideMiddle
            }
        }
    }

    // ---- pointer overlay: hover, click, rubber band, context menu ---------
    MouseArea {
        id: overlay

        anchors.fill: grid
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton

        property bool banding: false
        property real originX: 0
        property real originY: 0
        property int anchorIndex: -1

        function indexAt(mx, my) {
            return grid.indexAt(mx + grid.contentX, my + grid.contentY)
        }

        onPositionChanged: (mouse) => {
            if (banding) {
                band.update(mouse.x, mouse.y)
                return
            }
            root.hoveredIndex = indexAt(mouse.x, mouse.y)
        }

        onExited: root.hoveredIndex = -1

        onPressed: (mouse) => {
            const index = indexAt(mouse.x, mouse.y)

            if (mouse.button === Qt.RightButton) {
                if (index >= 0 && root.fsModel && !root.fsModel.isSelected(index))
                    root.fsModel.selectOnly(index)
                contextMenu.x = mouse.x
                contextMenu.y = mouse.y
                contextMenu.open()
                return
            }

            if (index < 0) {
                if (!(mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) && root.fsModel)
                    root.fsModel.clearSelection()
                banding = true
                originX = mouse.x
                originY = mouse.y
                band.update(mouse.x, mouse.y)
                return
            }

            if (!root.fsModel)
                return

            if (mouse.modifiers & Qt.ControlModifier) {
                root.fsModel.toggleSelected(index)
            } else if (mouse.modifiers & Qt.ShiftModifier) {
                const from = anchorIndex >= 0 ? anchorIndex : index
                root.fsModel.selectRange(from, index, false)
            } else {
                root.fsModel.selectOnly(index)
                anchorIndex = index
            }
            if (!(mouse.modifiers & Qt.ShiftModifier))
                anchorIndex = index
        }

        onReleased: {
            if (!banding)
                return
            banding = false
            band.visible = false
        }

        onDoubleClicked: (mouse) => {
            const index = indexAt(mouse.x, mouse.y)
            if (index >= 0)
                root.itemActivated(index)
        }

        onWheel: (wheel) => {
            const step = wheel.angleDelta.y !== 0 ? wheel.angleDelta.y : wheel.angleDelta.x
            const max = Math.max(0, grid.contentHeight - grid.height)
            grid.contentY = Math.max(0, Math.min(max, grid.contentY - step))
        }

        // Rubber band.
        Rectangle {
            id: band

            visible: false
            color: Qt.alpha(Theme.accent, 0.10)
            border.width: 1
            border.color: Theme.accent
            radius: 2

            function update(mx, my) {
                x = Math.min(overlay.originX, mx)
                y = Math.min(overlay.originY, my)
                width = Math.abs(mx - overlay.originX)
                height = Math.abs(my - overlay.originY)
                visible = width > 3 || height > 3
                if (visible)
                    applySelection()
            }

            function applySelection() {
                if (!root.fsModel)
                    return
                const hits = []
                const left = x + grid.contentX
                const top = y + grid.contentY
                const right = left + width
                const bottom = top + height
                const columns = Math.max(1, Math.floor(grid.width / grid.cellWidth))

                for (let i = 0; i < root.fsModel.count; ++i) {
                    const cx = (i % columns) * grid.cellWidth
                    const cy = Math.floor(i / columns) * grid.cellHeight
                    if (cx < right && cx + grid.cellWidth > left
                            && cy < bottom && cy + grid.cellHeight > top)
                        hits.push(i)
                }
                root.fsModel.selectIndices(hits, false)
            }
        }
    }

    // ---- context menu ----------------------------------------------------
    Popup {
        id: contextMenu

        padding: 5
        width: 244
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: Theme.surface
            radius: 9
            border.width: 1
            border.color: Theme.neutral700
        }

        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durPopup; easing.type: Easing.OutCubic }
                NumberAnimation { property: "y"; from: contextMenu.y - 6; to: contextMenu.y; duration: Theme.durPopup; easing.type: Easing.OutCubic }
            }
        }

        readonly property var entries: [
            { label: qsTr("Open in %1 new tabs").arg(Math.max(1, root.fsModel ? root.fsModel.selectionCount : 1)), action: "open.tabs" },
            { label: qsTr("Open as sequence"),      action: "open.sequence" },
            { label: qsTr("Compare side by side"),  action: "open.compare" },
            { label: "-" },
            { label: qsTr("Copy paths"),            action: "select.copyPaths" },
            { label: qsTr("Add tag…"),              action: "tag.add" },
            { label: "-" },
            { label: qsTr("Move to trash"),         action: "file.trash", danger: true },
            { label: qsTr("Delete permanently…"),   action: "file.delete", danger: true }
        ]

        contentItem: Column {
            spacing: 1

            Repeater {
                model: contextMenu.entries

                delegate: Loader {
                    required property var modelData
                    sourceComponent: modelData.label === "-" ? sep : entry

                    Component {
                        id: sep
                        Item {
                            width: 234
                            height: 7
                            Rectangle {
                                anchors.centerIn: parent
                                width: parent.width - 8
                                height: 1
                                color: Theme.borderSoft
                            }
                        }
                    }

                    Component {
                        id: entry
                        Rectangle {
                            width: 234
                            height: 24
                            radius: 5
                            color: hover.hovered ? Theme.tintFill : "transparent"
                            border.width: hover.hovered ? 1 : 0
                            border.color: Theme.tintRing
                            Behavior on color { ColorAnimation { duration: Theme.durHover } }

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 9
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.label
                                color: modelData.danger === true ? Theme.accent300 : Theme.bodyText
                                font.family: Theme.uiFamily
                                font.pixelSize: 11
                            }

                            HoverHandler { id: hover }
                            TapHandler {
                                onTapped: {
                                    contextMenu.close()
                                    root.contextAction(modelData.action)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
