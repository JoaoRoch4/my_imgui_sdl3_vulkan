import QtQuick
import QtMultimedia
import VulkanMedia
import "Format.js" as Format

/*! Video surface backed by Qt Multimedia. The OSD chrome is unchanged from the
    scaffold — only its data sources are real now. The mpv/libplacebo zero-copy
    path is still the eventual target; this gets pixels on screen today. */
Rectangle {
    id: root

    //! Absolute filesystem path of the clip. Empty shows the idle placeholder.
    property string path: ""
    //! Panels do not reach up at the window; they name an action, as FileGrid does.
    signal actionRequested(string action)

    readonly property string clipName: path.length > 0
                                       ? path.slice(path.lastIndexOf("/") + 1)
                                       : qsTr("No clip loaded")
    readonly property bool playing: player.playbackState === MediaPlayer.PlayingState
    property string errorText: ""

    //! 0..1 while the user drags the seek handle, -1 when they are not.
    property real dragFraction: -1
    readonly property real progress: dragFraction >= 0
                                     ? dragFraction
                                     : (player.duration > 0 ? player.position / player.duration : 0)

    /*!
        Decides when a drag actually moves the playhead. Called continuously
        while the user drags the seek handle, then once more on release with
        \a dragging false. \a fraction is 0..1 of the clip duration.

        The handle itself always tracks the cursor (dragFraction does that), so
        this governs the decoder work, not the visual feedback.
    */
    function applySeek(fraction, dragging) {
        // TODO(human)
    }

    color: Theme.bg

    onPathChanged: {
        errorText = ""
        dragFraction = -1
    }

    MediaPlayer {
        id: player

        // Local absolute paths only; QUrl parses the space-bearing ones fine.
        source: root.path.length > 0 ? "file://" + root.path : ""
        videoOutput: videoSurface
        audioOutput: AudioOutput { id: audioOut; muted: false }

        // Autoplay hangs off the player's own signal, not root.onPathChanged:
        // that handler and this source binding both react to root.path, and QML
        // orders them arbitrarily, so play() there can fire while source is
        // still empty — a silent no-op that leaves the clip loaded but stopped.
        onSourceChanged: if (source.toString().length > 0) play()

        onErrorOccurred: (error, errorString) => root.errorText = errorString
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 14
        radius: Theme.radius
        color: Theme.surface
        border.width: 1
        border.color: Theme.border
        clip: true

        VideoOutput {
            id: videoSurface

            anchors.fill: parent
            anchors.margins: 1
            fillMode: VideoOutput.PreserveAspectFit
            visible: root.path.length > 0 && root.errorText.length === 0
        }

        // Idle / error state. Kept from the scaffold so an empty Video tab still
        // reads as deliberate rather than broken.
        Column {
            anchors.centerIn: parent
            spacing: 7
            visible: !videoSurface.visible

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.errorText.length > 0 ? "⚠" : "▶"
                color: Theme.neutral700
                font.family: Theme.uiFamily
                font.pixelSize: 34
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.errorText.length > 0 ? qsTr("Cannot play this file")
                                                : qsTr("Open a video from the file grid")
                color: Theme.mutedText
                font.family: Theme.uiFamily
                font.pixelSize: 12
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(implicitWidth, root.width - 80)
                elide: Text.ElideMiddle
                text: root.errorText.length > 0 ? root.errorText
                                                : qsTr("Qt Multimedia · mpv / libplacebo not wired yet")
                color: Theme.neutral700
                font.family: Theme.monoFamily
                font.pixelSize: 10
            }
        }

        // Click the surface to toggle playback, the way every other player does.
        TapHandler {
            enabled: videoSurface.visible
            onTapped: root.playing ? player.pause() : player.play()
        }

        // OSD chrome.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 1
            height: 46
            color: Qt.alpha(Theme.bg, 0.85)
            radius: Theme.radius

            Row {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 10

                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: root.playing ? "⏸" : "▶"
                    glyphSize: 13
                    enabled: player.hasVideo || player.hasAudio
                    onClicked: root.playing ? player.pause() : player.play()
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Format.duration(root.progress * player.duration)
                    color: Theme.bodyText
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsSmall
                }

                // Seek bar — accent as a line, never a large fill.
                Item {
                    id: seekBar

                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 260
                    height: 16

                    function fractionAt(x) {
                        return Math.max(0, Math.min(1, x / width))
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: 2
                        radius: 1
                        color: Theme.border
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width * root.progress
                        height: 2
                        radius: 1
                        color: Theme.accent
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        x: parent.width * root.progress - 4
                        width: 8
                        height: 8
                        radius: 4
                        color: Theme.accent400
                        scale: seekArea.containsMouse || seekArea.pressed ? 1.35 : 1

                        Behavior on scale {
                            NumberAnimation { duration: Theme.durHover; easing.type: Easing.OutQuad }
                        }
                    }

                    MouseArea {
                        id: seekArea

                        anchors.fill: parent
                        anchors.topMargin: -6
                        anchors.bottomMargin: -6
                        hoverEnabled: true
                        enabled: player.seekable

                        onPressed: (mouse) => {
                            root.dragFraction = seekBar.fractionAt(mouse.x)
                            root.applySeek(root.dragFraction, true)
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed)
                                return
                            root.dragFraction = seekBar.fractionAt(mouse.x)
                            root.applySeek(root.dragFraction, true)
                        }
                        onReleased: (mouse) => {
                            const f = seekBar.fractionAt(mouse.x)
                            root.applySeek(f, false)
                            root.dragFraction = -1
                        }
                        onCanceled: root.dragFraction = -1
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Format.duration(player.duration)
                    color: Theme.neutral600
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fsSmall
                }

                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: audioOut.muted ? "🔇" : "♪"
                    glyphSize: 13
                    checkable: true
                    checked: audioOut.muted
                    onToggled: audioOut.muted = checked
                }

                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: "⛶"
                    glyphSize: 13
                    onClicked: root.actionRequested("window.fullscreen")
                }
            }
        }

        // Title badge.
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 10
            width: titleLabel.implicitWidth + 16
            height: 22
            radius: Theme.radiusSm
            color: Qt.alpha(Theme.bg, 0.85)
            border.width: 1
            border.color: Theme.border

            Text {
                id: titleLabel
                anchors.centerIn: parent
                text: Format.elideMiddle(root.clipName, 48)
                color: Theme.bodyText
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fsSmall
            }
        }
    }
}
