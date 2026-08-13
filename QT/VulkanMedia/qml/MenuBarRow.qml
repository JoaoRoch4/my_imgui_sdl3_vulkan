import QtQuick
import QtQuick.Controls.Basic
import VulkanMedia

/*! 29px application menu strip. The open menu carries the tinted accent-900
    fill with an accent-800 ring; dropdowns animate in with the shared
    opacity + rise transition. */
Rectangle {
    id: root

    property string appName: "VulkanMedia"
    property int activeMenu: -1

    signal actionTriggered(string action)
    signal panelsRequested(real x, real y)

    implicitHeight: 29
    color: Theme.chrome

    readonly property var menus: [
        { name: "File", items: [
            { label: "New tab",            shortcut: "Ctrl+T",       action: "tab.new" },
            { label: "Reopen closed tab",  shortcut: "Ctrl+Shift+T", action: "tab.reopen" },
            { label: "Open folder…",       shortcut: "Ctrl+O",       action: "folder.open" },
            { label: "-" },
            { label: "Quit",               shortcut: "Ctrl+Q",       action: "app.quit" } ] },
        { name: "Edit", items: [
            { label: "Select all",         shortcut: "Ctrl+A",       action: "select.all" },
            { label: "Clear selection",    shortcut: "Esc",          action: "select.none" },
            { label: "Copy paths",         shortcut: "Ctrl+C",       action: "select.copyPaths" } ] },
        { name: "View", items: [
            { label: "List",               shortcut: "",             action: "view.list" },
            { label: "Grid",               shortcut: "",             action: "view.grid" },
            { label: "Masonry",            shortcut: "",             action: "view.masonry" },
            { label: "-" },
            { label: "Toggle details",     shortcut: "Ctrl+D",       action: "panel.details" } ] },
        { name: "Panels", items: [] },
        { name: "Tools", items: [
            { label: "Downloads",          shortcut: "",             action: "panel.downloads" },
            { label: "Metadata editor",    shortcut: "",             action: "panel.metadata" },
            { label: "Thread reflection",  shortcut: "",             action: "panel.threads" },
            { label: "-" },
            { label: "Console",            shortcut: "F1",           action: "panel.console" },
            { label: "Runtime config",     shortcut: "F2",           action: "panel.config" } ] },
        { name: "Window", items: [
            { label: "Reset layout",       shortcut: "",             action: "layout.reset" },
            { label: "Toggle full screen", shortcut: "F11",          action: "window.fullscreen" } ] },
        { name: "Help", items: [
            { label: "Keyboard shortcuts", shortcut: "",             action: "help.shortcuts" },
            { label: "About VulkanMedia",  shortcut: "",             action: "help.about" } ] }
    ]

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.borderSoft
    }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.appName
            color: Theme.text
            font.family: Theme.uiFamily
            font.pixelSize: Theme.fsBody
            font.weight: Font.Medium
            rightPadding: 10
        }

        Repeater {
            model: root.menus

            delegate: Item {
                id: entry
                required property int index
                required property var modelData

                width: caption.implicitWidth + 16
                height: 20
                anchors.verticalCenter: parent.verticalCenter

                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: root.activeMenu === entry.index ? Theme.tintFill
                         : hover.hovered ? Qt.alpha(Theme.accent, 0.10)
                         : "transparent"
                    border.width: root.activeMenu === entry.index ? 1 : 0
                    border.color: Theme.tintRing
                    Behavior on color { ColorAnimation { duration: Theme.durHover } }
                }

                Text {
                    id: caption
                    anchors.centerIn: parent
                    text: entry.modelData.name
                    color: root.activeMenu === entry.index ? Theme.text : Theme.bodyText
                    font.family: Theme.uiFamily
                    font.pixelSize: 11
                }

                HoverHandler { id: hover }

                TapHandler {
                    onTapped: {
                        if (entry.modelData.name === "Panels") {
                            root.activeMenu = -1
                            dropdown.close()
                            const p = entry.mapToItem(null, 0, entry.height + 4)
                            root.panelsRequested(p.x, p.y)
                            return
                        }
                        if (root.activeMenu === entry.index) {
                            root.activeMenu = -1
                            dropdown.close()
                        } else {
                            root.activeMenu = entry.index
                            dropdown.items = entry.modelData.items
                            const p = entry.mapToItem(root, 0, entry.height + 4)
                            dropdown.x = p.x
                            dropdown.y = p.y
                            dropdown.open()
                        }
                    }
                }
            }
        }
    }

    Popup {
        id: dropdown

        property var items: []

        padding: 5
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: root.activeMenu = -1

        background: Rectangle {
            color: Theme.surface
            radius: 9
            border.width: 1
            border.color: Theme.neutral700
        }

        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durPopup; easing.type: Easing.OutCubic }
                NumberAnimation { property: "y"; from: dropdown.y - 6; to: dropdown.y; duration: Theme.durPopup; easing.type: Easing.OutCubic }
            }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; to: 0; duration: 90 }
        }

        contentItem: Column {
            spacing: 1

            Repeater {
                model: dropdown.items

                delegate: Loader {
                    required property var modelData
                    sourceComponent: modelData.label === "-" ? separatorComp : itemComp

                    Component {
                        id: separatorComp
                        Item {
                            width: 210
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
                        id: itemComp
                        Rectangle {
                            width: 210
                            height: 24
                            radius: 5
                            color: rowHover.hovered ? Theme.tintFill : "transparent"
                            border.width: rowHover.hovered ? 1 : 0
                            border.color: Theme.tintRing
                            Behavior on color { ColorAnimation { duration: Theme.durHover } }

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 9
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.label
                                color: Theme.bodyText
                                font.family: Theme.uiFamily
                                font.pixelSize: 11
                            }

                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: 9
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.shortcut || ""
                                color: Theme.neutral600
                                font.family: Theme.monoFamily
                                font.pixelSize: Theme.fsMicro
                            }

                            HoverHandler { id: rowHover }
                            TapHandler {
                                onTapped: {
                                    dropdown.close()
                                    root.actionTriggered(modelData.action)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
