import QtQuick
import QtQuick.Controls.Basic
import VulkanMedia

/*! 36px navigation strip: history buttons, breadcrumb, sort, view mode and
    search. */
Rectangle {
    id: root

    property bool canGoBack: true
    property bool canGoForward: false
    property var pathSegments: ["🏠", "media", "reference"]
    property string sortKey: "Name"
    property int viewMode: 1 //!< 0 list, 1 grid, 2 masonry
    property alias searchText: search.text
    property string searchScope: "reference"

    signal back()
    signal forward()
    signal up()
    signal sortRequested()
    //! Not `viewModeChanged` — that name belongs to the viewMode property.
    signal viewModeSelected(int mode)
    signal crumbActivated(int index)

    implicitHeight: 36
    color: Theme.chrome

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.borderSoft
    }

    component Separator: Rectangle {
        width: 1
        height: 17
        color: Theme.border
        anchors.verticalCenter: parent.verticalCenter
    }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        Row {
            spacing: 2
            anchors.verticalCenter: parent.verticalCenter

            IconButton {
                glyph: "‹"
                glyphSize: 15
                enabled: root.canGoBack
                onClicked: root.back()
            }
            IconButton {
                glyph: "›"
                glyphSize: 15
                enabled: root.canGoForward
                onClicked: root.forward()
            }
            IconButton {
                glyph: "↑"
                glyphSize: 15
                onClicked: root.up()
            }
        }

        Separator {}

        // Breadcrumb pill.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            height: 26
            width: crumbs.implicitWidth + 20
            radius: Theme.radiusSm
            color: Theme.bg
            border.width: 1
            border.color: Theme.border

            Row {
                id: crumbs
                anchors.centerIn: parent
                spacing: 5

                Repeater {
                    model: root.pathSegments

                    delegate: Row {
                        required property int index
                        required property string modelData
                        spacing: 5

                        Text {
                            text: "/"
                            visible: index > 0
                            color: Theme.neutral700
                            font.family: Theme.uiFamily
                            font.pixelSize: 11
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            text: modelData
                            color: index === root.pathSegments.length - 1 ? Theme.text : Theme.mutedText
                            font.family: Theme.uiFamily
                            font.pixelSize: 11
                            font.weight: index === root.pathSegments.length - 1 ? Font.Medium : Font.Normal
                            anchors.verticalCenter: parent.verticalCenter

                            TapHandler { onTapped: root.crumbActivated(index) }
                        }
                    }
                }
            }
        }

        Separator {}

        // Sort pill — outlined, never a solid accent fill.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            height: 26
            width: sortRow.implicitWidth + 20
            radius: Theme.radiusSm
            color: sortHover.hovered ? Qt.alpha(Theme.accent, 0.10) : "transparent"
            border.width: 1
            border.color: Theme.border
            Behavior on color { ColorAnimation { duration: Theme.durHover } }

            Row {
                id: sortRow
                anchors.centerIn: parent
                spacing: 6

                Text {
                    text: root.sortKey
                    color: Theme.bodyText
                    font.family: Theme.uiFamily
                    font.pixelSize: 11
                }
                Text {
                    text: "⌄"
                    color: Theme.neutral600
                    font.family: Theme.uiFamily
                    font.pixelSize: 11
                }
            }

            HoverHandler { id: sortHover }
            TapHandler { onTapped: root.sortRequested() }
        }

        SegmentedControl {
            anchors.verticalCenter: parent.verticalCenter
            currentIndex: root.viewMode
            options: [
                { glyph: "☰", name: "list" },
                { glyph: "▦", name: "grid" },
                { glyph: "▥", name: "masonry" }
            ]
            onActivated: (index) => root.viewModeSelected(index)
        }
    }

    // Search field.
    TextField {
        id: search
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        width: 190
        height: 26
        placeholderText: qsTr("Search ") + root.searchScope
        placeholderTextColor: Theme.neutral600
        color: Theme.bodyText
        font.family: Theme.uiFamily
        font.pixelSize: 11
        leftPadding: 9
        rightPadding: 9
        verticalAlignment: TextInput.AlignVCenter
        selectionColor: Qt.alpha(Theme.accent, 0.35)
        selectedTextColor: Theme.text

        background: Rectangle {
            radius: Theme.radiusSm
            color: Theme.bg
            border.width: 1
            border.color: search.activeFocus ? Theme.accent : Theme.border
            Behavior on border.color { ColorAnimation { duration: Theme.durHover } }
        }
    }
}
