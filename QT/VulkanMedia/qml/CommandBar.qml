import QtQuick
import QtQuick.Controls.Basic
import VulkanMedia

/*! 34px command line. Typing filters the command list into a popup that rises
    above the bar; ↑/↓ walk the history. */
Rectangle {
    id: root

    property var history: []
    property int historyCursor: -1

    signal commandSubmitted(string text)

    implicitHeight: 34
    color: Theme.panel

    function focusInput() {
        input.forceActiveFocus()
        input.selectAll()
    }

    readonly property var commands: [
        { name: "open files",       hint: "focus the file browser panel" },
        { name: "open images",      hint: "focus the image viewer" },
        { name: "open video",       hint: "focus the video player" },
        { name: "open downloads",   hint: "focus the download queue" },
        { name: "open console",     hint: "focus the console (F1)" },
        { name: "open config",      hint: "focus runtime config (F2)" },
        { name: "open metadata",    hint: "focus the metadata editor" },
        { name: "open threads",     hint: "focus thread reflection" },
        { name: "config thumbnail.size",  hint: "set a runtime config key" },
        { name: "config player.hwdec",    hint: "set a runtime config key" },
        { name: "config grid.columns",    hint: "set a runtime config key" },
        { name: "tag",              hint: "tag the current selection" },
        { name: "download",         hint: "queue a URL for download" }
    ]

    readonly property var matches: {
        const q = input.text.trim().toLowerCase()
        if (q.length === 0)
            return []
        return commands.filter((c) => c.name.indexOf(q) >= 0)
    }

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.borderSoft
    }

    Row {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 8

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: ">"
            color: Theme.accent
            font.family: Theme.monoFamily
            font.pixelSize: 12
            font.weight: Font.Medium
        }

        TextInput {
            id: input
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 200
            color: Theme.bodyText
            font.family: Theme.monoFamily
            font.pixelSize: 12
            selectionColor: Qt.alpha(Theme.accent, 0.35)
            selectedTextColor: Theme.text
            clip: true

            cursorDelegate: Rectangle {
                width: 2
                color: Theme.accent
                visible: input.activeFocus

                SequentialAnimation on opacity {
                    running: input.activeFocus
                    loops: Animation.Infinite
                    NumberAnimation { to: 0; duration: 480; easing.type: Easing.InOutQuad }
                    NumberAnimation { to: 1; duration: 480; easing.type: Easing.InOutQuad }
                }
            }

            Keys.onUpPressed: root.walkHistory(1)
            Keys.onDownPressed: root.walkHistory(-1)
            Keys.onEscapePressed: {
                input.text = ""
                input.focus = false
            }
            Keys.onTabPressed: {
                if (root.matches.length > 0)
                    input.text = root.matches[0].name + " "
                input.cursorPosition = input.text.length
            }

            onAccepted: {
                const text = input.text.trim()
                if (text.length === 0)
                    return
                root.history = [text].concat(root.history).slice(0, 50)
                root.historyCursor = -1
                root.commandSubmitted(text)
                input.text = ""
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: input.text.length === 0 && !input.activeFocus
                text: qsTr("type a command — open, config, tag, download")
                color: Theme.neutral700
                font.family: Theme.monoFamily
                font.pixelSize: 12
            }
        }
    }

    Text {
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: qsTr("↑↓ history · Ctrl+` focus")
        color: Theme.neutral700
        font.family: Theme.monoFamily
        font.pixelSize: 10
    }

    function walkHistory(direction) {
        if (history.length === 0)
            return
        historyCursor = Math.max(-1, Math.min(history.length - 1, historyCursor + direction))
        input.text = historyCursor < 0 ? "" : history[historyCursor]
        input.cursorPosition = input.text.length
    }

    // Completion popup, rising above the bar.
    Popup {
        id: completions

        y: -height - 6
        x: 12
        width: 420
        padding: 5
        visible: root.matches.length > 0 && input.activeFocus
        closePolicy: Popup.NoAutoClose

        background: Rectangle {
            color: Theme.surface
            radius: 9
            border.width: 1
            border.color: Theme.neutral700
        }

        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durPopup; easing.type: Easing.OutCubic }
                NumberAnimation { property: "y"; from: completions.y + 6; to: completions.y; duration: Theme.durPopup; easing.type: Easing.OutCubic }
            }
        }

        contentItem: Column {
            spacing: 1

            Repeater {
                model: root.matches.slice(0, 7)

                delegate: Rectangle {
                    required property int index
                    required property var modelData

                    width: 410
                    height: 24
                    radius: 5
                    color: index === 0 ? Theme.tintFill : (hover.hovered ? Qt.alpha(Theme.accent, 0.10) : "transparent")
                    border.width: index === 0 ? 1 : 0
                    border.color: Theme.tintRing

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 9
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.name
                        color: Theme.text
                        font.family: Theme.monoFamily
                        font.pixelSize: 11
                    }

                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 9
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.hint
                        color: Theme.neutral600
                        font.family: Theme.uiFamily
                        font.pixelSize: 10
                    }

                    HoverHandler { id: hover }
                    TapHandler {
                        onTapped: {
                            input.text = modelData.name
                            input.cursorPosition = input.text.length
                            input.forceActiveFocus()
                        }
                    }
                }
            }
        }
    }
}
