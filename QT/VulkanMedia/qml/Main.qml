import QtCore
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import VulkanMedia

ApplicationWindow {
    id: window

    width: 1180
    height: 748
    minimumWidth: 820
    minimumHeight: 520
    visible: true
    color: Theme.bg
    title: qsTr("VulkanMedia — %1").arg(fsModel.folderName)

    // ---- state -----------------------------------------------------------
    property string activePanel: "files"
    property bool leftDockOpen: true
    property bool detailsOpen: true
    property bool detailsPinned: false
    property int viewMode: 1
    property var closedTabs: []
    property var openPanels: ["files", "details", "console"]
    //! Clip currently loaded in the video panel. Not persisted — reopening the
    //! app should not silently start decoding whatever was last played.
    property string videoPath: ""

    readonly property var panelTitles: ({
        files: "Files", images: "Images", video: "Video", downloads: "Downloads",
        console: "Console", config: "Config", metadata: "Metadata", threads: "Threads"
    })
    readonly property var panelGlyphs: ({
        files: "🗂", images: "🖼", video: "▶", downloads: "⤓",
        console: "▤", config: "⚙", metadata: "✎", threads: "⑃"
    })

    FileSystemModel {
        id: fsModel
        folder: settings.lastFolder
        onFocusedIndexChanged: window.syncFocusedItem()
        onCountChanged: window.syncFocusedItem()
    }

    // Layout persistence.
    Settings {
        id: settings
        category: "layout"
        property string lastFolder: ""
        property bool leftDockOpen: true
        property bool detailsOpen: true
        property string activePanel: "files"
        property int viewMode: 1
        property string openPanels: "files,details,console"
    }

    property var focusedItem: ({})

    function syncFocusedItem() {
        focusedItem = fsModel.focusedIndex >= 0 ? fsModel.get(fsModel.focusedIndex) : ({})
    }

    Component.onCompleted: {
        leftDockOpen = settings.leftDockOpen
        detailsOpen = settings.detailsOpen
        activePanel = settings.activePanel
        viewMode = settings.viewMode
        openPanels = settings.openPanels.split(",").filter((s) => s.length > 0)
        syncFocusedItem()
    }

    Component.onDestruction: {
        settings.lastFolder = fsModel.folder
        settings.leftDockOpen = leftDockOpen
        settings.detailsOpen = detailsOpen
        settings.activePanel = activePanel
        settings.viewMode = viewMode
        settings.openPanels = openPanels.join(",")
    }

    // ---- actions ---------------------------------------------------------
    function openPanel(panel) {
        activePanel = panel
        if (openPanels.indexOf(panel) < 0)
            openPanels = openPanels.concat([panel])
        const existing = indexOfTab(panel)
        if (existing >= 0)
            tabs.currentIndex = existing
        else
            addTab(panel)
    }

    /*! Video rows open in the video panel; everything else keeps the old
        folder-entering behaviour. Stub rows name paths that do not exist — they
        are left to the player's error state rather than special-cased here, so
        the failure a user sees is the real one. */
    function activateItem(index) {
        const item = fsModel.get(index)
        if (item.kind === "video") {
            videoPath = item.path
            openPanel("video")
            return
        }
        fsModel.enter(index)
    }

    function indexOfTab(panel) {
        for (let i = 0; i < tabModel.count; ++i) {
            if (tabModel.get(i).panel === panel)
                return i
        }
        return -1
    }

    function addTab(panel) {
        tabModel.append({
            title: panelTitles[panel] || panel,
            glyph: panelGlyphs[panel] || "▦",
            panel: panel
        })
        tabs.currentIndex = tabModel.count - 1
    }

    function closeTab(index) {
        if (tabModel.count <= 1)
            return
        closedTabs = [tabModel.get(index).panel].concat(closedTabs).slice(0, 12)
        tabModel.remove(index)
        tabs.currentIndex = Math.min(index, tabModel.count - 1)
        activePanel = tabModel.get(tabs.currentIndex).panel
    }

    function reopenTab() {
        if (closedTabs.length === 0)
            return
        const panel = closedTabs[0]
        closedTabs = closedTabs.slice(1)
        addTab(panel)
        activePanel = panel
    }

    function togglePanel(panel) {
        const i = openPanels.indexOf(panel)
        if (i >= 0 && panel !== activePanel)
            openPanels = openPanels.filter((p) => p !== panel)
        else
            openPanel(panel)
    }

    function runAction(action) {
        switch (action) {
        case "tab.new":            addTab(activePanel); break
        case "tab.reopen":         reopenTab(); break
        case "folder.open":        folderDialog.open(); break
        case "app.quit":           Qt.quit(); break
        case "select.all":         fsModel.selectAll(); break
        case "select.none":        fsModel.clearSelection(); break
        case "select.copyPaths":   console.log("copy paths:", fsModel.selectedPaths().join("\n")); break
        case "view.list":          viewMode = 0; break
        case "view.grid":          viewMode = 1; break
        case "view.masonry":       viewMode = 2; break
        case "panel.details":      detailsOpen = !detailsOpen; break
        case "layout.reset":       resetLayout(); break
        case "window.fullscreen":  window.visibility = window.visibility === Window.FullScreen
                                                       ? Window.Windowed : Window.FullScreen; break
        case "file.trash":         fsModel.trashSelected(); break
        case "file.delete":        deleteDialog.open(); break
        case "open.tabs":          addTab(activePanel); break
        case "tag.add":            commandBar.focusInput(); break
        default:
            if (action.indexOf("panel.") === 0)
                openPanel(action.substring(6))
            else
                console.log("unhandled action:", action)
        }
    }

    function resetLayout() {
        leftDockOpen = true
        detailsOpen = true
        detailsPinned = false
        viewMode = 1
        openPanels = ["files", "details", "console"]
    }

    function runCommand(text) {
        const parts = text.trim().split(/\s+/)
        if (parts[0] === "open" && parts.length > 1)
            openPanel(parts[1])
        else if (parts[0] === "config")
            openPanel("config")
        else if (parts[0] === "tag")
            openPanel("metadata")
        else if (parts[0] === "download")
            openPanel("downloads")
        else
            console.log("unknown command:", text)
    }

    // ---- shortcuts -------------------------------------------------------
    Shortcut { sequence: "Alt+1"; onActivated: window.focusDock("left") }
    Shortcut { sequence: "Alt+2"; onActivated: window.focusDock("right") }
    Shortcut { sequence: "Alt+3"; onActivated: window.focusDock("bottom") }
    Shortcut { sequence: "Alt+4"; onActivated: window.focusDock("center") }
    Shortcut { sequence: "Ctrl+1"; onActivated: window.openPanel("files") }
    Shortcut { sequence: "Ctrl+2"; onActivated: window.openPanel("images") }
    Shortcut { sequence: "Ctrl+3"; onActivated: window.openPanel("video") }
    Shortcut { sequence: "Ctrl+T"; onActivated: window.addTab(window.activePanel) }
    Shortcut { sequence: "Ctrl+Shift+T"; onActivated: window.reopenTab() }
    Shortcut { sequence: "Ctrl+D"; onActivated: window.detailsOpen = !window.detailsOpen }
    Shortcut { sequence: "Ctrl+A"; onActivated: fsModel.selectAll() }
    Shortcut { sequence: "Ctrl+O"; onActivated: folderDialog.open() }
    Shortcut { sequence: "Ctrl+Q"; onActivated: Qt.quit() }
    Shortcut { sequence: "F1"; onActivated: window.openPanel("console") }
    Shortcut { sequence: "F2"; onActivated: window.openPanel("config") }
    Shortcut { sequence: "F11"; onActivated: window.runAction("window.fullscreen") }
    Shortcut { sequence: "Ctrl+`"; onActivated: commandBar.focusInput() }
    Shortcut { sequence: "Ctrl+Shift+P"; onActivated: window.showPanelMenu() }
    Shortcut { sequence: "Delete"; onActivated: fsModel.trashSelected() }
    Shortcut { sequence: "Shift+Delete"; onActivated: deleteDialog.open() }
    Shortcut { sequence: "Escape"; onActivated: fsModel.clearSelection() }

    function focusDock(dock) {
        switch (dock) {
        case "left":   launcher.forceActiveFocus(Qt.TabFocusReason); break
        case "right":  details.forceActiveFocus(Qt.TabFocusReason); break
        case "bottom": commandBar.focusInput(); break
        case "center": centerColumn.forceActiveFocus(Qt.TabFocusReason); break
        }
    }

    function showPanelMenu() {
        panelMenu.x = Math.round((window.width - panelMenu.width) / 2)
        panelMenu.y = 40
        panelMenu.open()
    }

    // ---- layout ----------------------------------------------------------
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        MenuBarRow {
            Layout.fillWidth: true
            onActionTriggered: (action) => window.runAction(action)
            onPanelsRequested: (x, y) => {
                panelMenu.x = Math.min(x, window.width - panelMenu.width - 8)
                panelMenu.y = y
                panelMenu.open()
            }
        }

        ToolBarRow {
            id: toolbar
            Layout.fillWidth: true
            viewMode: window.viewMode
            pathSegments: ["🏠", "media", fsModel.folderName]
            searchScope: fsModel.folderName
            onViewModeSelected: (mode) => window.viewMode = mode
            onUp: fsModel.goUp()
            onBack: fsModel.goUp()
            onSortRequested: console.log("sort menu is stubbed")
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            LauncherPanel {
                id: launcher

                Layout.fillHeight: true
                Layout.preferredWidth: window.leftDockOpen ? 176 : 0
                visible: Layout.preferredWidth > 0.5
                clip: true
                activePanel: window.activePanel
                currentFolder: fsModel.folderName
                onPanelActivated: (panel) => window.openPanel(panel)
                onCollapseRequested: window.leftDockOpen = false
                onPlaceActivated: (path) => fsModel.folder = path

                Behavior on Layout.preferredWidth {
                    NumberAnimation { duration: Theme.durPanel; easing.type: Easing.OutCubic }
                }
            }

            ColumnLayout {
                id: centerColumn
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // ---- tab strip ----
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 29
                    color: Theme.chrome

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: Theme.borderSoft
                    }

                    ListModel {
                        id: tabModel
                        ListElement { title: "Files"; glyph: "🗂"; panel: "files" }
                        ListElement { title: "Console"; glyph: "▤"; panel: "console" }
                    }

                    Row {
                        id: tabs
                        property int currentIndex: 0

                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        onCurrentIndexChanged: {
                            if (currentIndex >= 0 && currentIndex < tabModel.count)
                                window.activePanel = tabModel.get(currentIndex).panel
                        }

                        Repeater {
                            model: tabModel

                            delegate: Rectangle {
                                id: tab
                                required property int index
                                required property string title
                                required property string glyph

                                readonly property bool current: tabs.currentIndex === index

                                width: tabRow.implicitWidth + 20
                                height: 22
                                radius: 5
                                color: tab.current ? Theme.bg
                                     : tabHover.hovered ? Qt.alpha(Theme.accent, 0.08)
                                     : "transparent"
                                border.width: tab.current ? 1 : 0
                                border.color: Theme.border
                                Behavior on color { ColorAnimation { duration: Theme.durHover } }

                                Row {
                                    id: tabRow
                                    anchors.centerIn: parent
                                    spacing: 6

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: tab.glyph
                                        color: tab.current ? Theme.accent400 : Theme.mutedText
                                        font.family: Theme.uiFamily
                                        font.pixelSize: 11
                                    }

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: tab.title
                                        color: tab.current ? Theme.text : Theme.bodyText
                                        font.family: Theme.uiFamily
                                        font.pixelSize: 11
                                    }

                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "✕"
                                        color: closeHover.hovered ? Theme.accent300 : Theme.neutral600
                                        font.family: Theme.uiFamily
                                        font.pixelSize: 9

                                        HoverHandler { id: closeHover }
                                        TapHandler { onTapped: window.closeTab(tab.index) }
                                    }
                                }

                                HoverHandler { id: tabHover }
                                TapHandler { onTapped: tabs.currentIndex = tab.index }
                            }
                        }

                        IconButton {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 22
                            height: 22
                            glyph: "+"
                            glyphSize: 12
                            onClicked: window.addTab(window.activePanel)
                        }
                    }
                }

                // ---- panel stack ----
                StackLayout {
                    id: stack

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: {
                        const order = ["files", "images", "video", "downloads",
                                       "console", "config", "metadata", "threads"]
                        const i = order.indexOf(window.activePanel)
                        return i < 0 ? 0 : i
                    }

                    component Page: Item {
                        opacity: StackLayout.isCurrentItem ? 1 : 0
                        Behavior on opacity {
                            NumberAnimation { duration: Theme.durTab; easing.type: Easing.OutQuad }
                        }
                    }

                    Page {
                        FileGrid {
                            anchors.fill: parent
                            fsModel: fsModel
                            folderName: fsModel.folderName
                            filter: toolbar.searchText
                            onItemActivated: (index) => window.activateItem(index)
                            onContextAction: (action) => window.runAction(action)
                        }
                    }
                    Page { PlaceholderPanel { anchors.fill: parent; title: qsTr("Image viewer"); glyph: "🖼" } }
                    Page {
                        VideoPanel {
                            anchors.fill: parent
                            path: window.videoPath
                            onActionRequested: (action) => window.runAction(action)
                        }
                    }
                    Page { PlaceholderPanel { anchors.fill: parent; title: qsTr("Downloads"); glyph: "⤓" } }
                    Page { PlaceholderPanel { anchors.fill: parent; title: qsTr("Console"); glyph: "▤" } }
                    Page { PlaceholderPanel { anchors.fill: parent; title: qsTr("Runtime config"); glyph: "⚙" } }
                    Page { PlaceholderPanel { anchors.fill: parent; title: qsTr("Metadata editor"); glyph: "✎" } }
                    Page { PlaceholderPanel { anchors.fill: parent; title: qsTr("Thread reflection"); glyph: "⑃" } }
                }
            }

            DetailsPanel {
                id: details

                Layout.fillHeight: true
                Layout.preferredWidth: window.detailsOpen ? 252 : 0
                visible: Layout.preferredWidth > 0.5
                clip: true
                fsModel: fsModel
                pinned: window.detailsPinned
                itemName: window.focusedItem.name !== undefined ? window.focusedItem.name : "—"
                itemPath: window.focusedItem.path !== undefined ? window.focusedItem.path : ""
                itemFormat: window.focusedItem.format !== undefined ? window.focusedItem.format : ""
                itemThumbnail: window.focusedItem.thumbnail !== undefined ? window.focusedItem.thumbnail : ""
                itemSize: window.focusedItem.size !== undefined ? window.focusedItem.size : 0
                itemDimensions: window.focusedItem.dimensions !== undefined ? window.focusedItem.dimensions : ""
                itemModified: window.focusedItem.modified

                onCollapseRequested: window.detailsOpen = false
                onPinToggled: window.detailsPinned = !window.detailsPinned
                onOpenRequested: fsModel.enter(fsModel.focusedIndex)

                Behavior on Layout.preferredWidth {
                    NumberAnimation { duration: Theme.durPanel; easing.type: Easing.OutCubic }
                }
            }
        }

        CommandBar {
            id: commandBar
            Layout.fillWidth: true
            onCommandSubmitted: (text) => window.runCommand(text)
        }

        StatusBar {
            Layout.fillWidth: true
            selectedCount: fsModel.selectionCount
            totalCount: fsModel.count
            selectionBytes: fsModel.selectionBytes
            thumbsReady: ThumbnailCache.ready
            thumbsTotal: ThumbnailCache.total
            downloadCount: 2
        }
    }

    // ---- overlays --------------------------------------------------------
    PanelMenu {
        id: panelMenu
        openPanels: window.openPanels
        onPanelToggled: (panel) => window.togglePanel(panel)
        onPanelPlacement: (panel, placement) => {
            console.log("placement is stubbed:", panel, "→", placement)
            window.openPanel(panel)
        }
        onLayoutReset: window.resetLayout()
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Open folder")
        onAccepted: fsModel.folder = selectedFolder
    }

    Dialog {
        id: deleteDialog

        anchors.centerIn: parent
        width: 360
        modal: true
        title: qsTr("Delete permanently")
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: fsModel.deleteSelected()

        background: Rectangle {
            color: Theme.surface
            radius: 9
            border.width: 1
            border.color: Theme.neutral700
        }

        header: Text {
            text: deleteDialog.title
            color: Theme.text
            font.family: Theme.uiFamily
            font.pixelSize: 13
            font.weight: Font.Medium
            padding: 14
        }

        contentItem: Text {
            text: qsTr("Delete %1 item(s) from disk? This cannot be undone.")
                      .arg(fsModel.selectionCount)
            color: Theme.bodyText
            font.family: Theme.uiFamily
            font.pixelSize: Theme.fsBody
            wrapMode: Text.WordWrap
        }
    }
}
