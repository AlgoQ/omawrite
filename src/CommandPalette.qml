import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One search box over both kinds of thing the writer reaches for: the
// backend's action registry and the slipbox's notes. Ctrl+P opens both; the
// Ctrl+Shift+O alias opens it with files only. A plain overlay rather than a
// Popup, so the text field owns the keyboard from the first keypress.
Item {
    id: paletteRoot
    objectName: "commandPalette"
    visible: false
    z: 110

    property bool darkMode: true
    property color textColor: "#d0d0d0"
    property color mutedColor: "#909191"
    property color selectionFill: "#428bca"
    property color selectionTextColor: "#ffffff"
    property real textScale: 1
    property string fontFamily: "IBM Plex Mono"
    // File-only mode is the old quick-open behaviour behind Ctrl+Shift+O.
    property bool fileMode: false
    property var rows: []
    readonly property int rowHeight: Math.max(1, Math.round(40 * textScale))

    signal commandChosen(string id)
    signal fileChosen(var url)
    signal closed

    function scaled(pixels) {
        return Math.max(1, Math.round(pixels * textScale));
    }

    function open(fileOnly) {
        fileMode = Boolean(fileOnly);
        visible = true;
        field.text = "";
        rebuild();
        Qt.callLater(function() {
            field.forceActiveFocus();
            field.cursorPosition = field.text.length;
        });
    }

    function close() {
        if (!visible)
            return;
        visible = false;
        field.text = "";
        closed();
    }

    function pageStep() {
        return Math.max(1, Math.floor(list.height / rowHeight));
    }

    function moveCurrent(delta) {
        if (rows.length === 0)
            return;
        var index = Math.max(0, Math.min(rows.length - 1, list.currentIndex + delta));
        list.currentIndex = index;
        list.positionViewAtIndex(index, ListView.Contain);
    }

    function activateCurrent() {
        if (list.currentIndex < 0 || list.currentIndex >= rows.length)
            return;
        var row = rows[list.currentIndex];
        close();
        if (row.kind === "command")
            commandChosen(row.id);
        else
            fileChosen(row.url);
    }

    function folderOf(path) {
        var slash = path.lastIndexOf("/");
        return slash >= 0 ? path.slice(0, slash) : "";
    }

    function rebuild() {
        var needle = field.text.trim().toLowerCase();
        var next = [];
        if (!fileMode) {
            var actions = backend.actionRows();
            for (var i = 0; i < actions.length; ++i) {
                var action = actions[i];
                if (needle !== ""
                        && action.label.toLowerCase().indexOf(needle) < 0
                        && action.id.toLowerCase().indexOf(needle) < 0
                        && action.sequence.toLowerCase().indexOf(needle) < 0)
                    continue;
                next.push({ kind: "command", id: action.id, label: action.label,
                            detail: action.sequence });
            }
        }
        var notes = backend.noteSuggestions(field.text);
        for (var j = 0; j < notes.length; ++j) {
            var note = notes[j];
            next.push({ kind: "file", url: note.url, label: note.name,
                        detail: folderOf(note.path) });
        }
        rows = next;
        list.currentIndex = next.length > 0 ? 0 : -1;
        list.positionViewAtBeginning();
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.5)

        MouseArea {
            anchors.fill: parent
            onClicked: paletteRoot.close()
        }
    }

    Rectangle {
        id: panel
        x: Math.round((paletteRoot.width - width) / 2)
        y: Math.round(paletteRoot.height * 0.12)
        width: Math.min(paletteRoot.scaled(620), paletteRoot.width - 48)
        height: Math.min(paletteRoot.scaled(420), paletteRoot.height - 160)
        color: paletteRoot.darkMode ? "#22221f" : "#fffef2"
        border.color: paletteRoot.darkMode ? "#4f525a" : "#d9d8c8"
        border.width: 1
        radius: 9

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            TextField {
                id: field
                objectName: "commandPaletteField"
                Layout.fillWidth: true
                placeholderText: paletteRoot.fileMode ? "Open other file…" : "Search commands and files…"
                placeholderTextColor: paletteRoot.mutedColor
                color: paletteRoot.textColor
                selectionColor: paletteRoot.selectionFill
                selectedTextColor: paletteRoot.selectionTextColor
                font.family: paletteRoot.fontFamily
                font.pixelSize: paletteRoot.scaled(17)
                leftPadding: paletteRoot.scaled(16)
                rightPadding: paletteRoot.scaled(16)
                topPadding: paletteRoot.scaled(14)
                bottomPadding: paletteRoot.scaled(14)
                background: Rectangle {
                    color: paletteRoot.darkMode ? "#22221f" : "#fffef2"
                }
                onTextChanged: paletteRoot.rebuild()
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Down) {
                        paletteRoot.moveCurrent(1);
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Up) {
                        paletteRoot.moveCurrent(-1);
                        event.accepted = true;
                    } else if (event.key === Qt.Key_PageDown) {
                        paletteRoot.moveCurrent(paletteRoot.pageStep());
                        event.accepted = true;
                    } else if (event.key === Qt.Key_PageUp) {
                        paletteRoot.moveCurrent(-paletteRoot.pageStep());
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        paletteRoot.activateCurrent();
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Escape) {
                        paletteRoot.close();
                        event.accepted = true;
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                height: 1
                color: paletteRoot.darkMode ? "#3a3a33" : "#e3e2d2"
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: list
                    objectName: "commandPaletteList"
                    anchors.fill: parent
                    anchors.topMargin: 4
                    anchors.bottomMargin: 4
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    highlightFollowsCurrentItem: true
                    model: paletteRoot.rows

                    delegate: Rectangle {
                        id: row
                        required property var modelData
                        required property int index
                        width: ListView.view.width
                        height: paletteRoot.rowHeight
                        radius: 6
                        color: index === list.currentIndex
                            ? (paletteRoot.darkMode ? "#35352f" : "#efeedc")
                            : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: paletteRoot.scaled(14)
                            anchors.rightMargin: paletteRoot.scaled(16)
                            spacing: paletteRoot.scaled(10)

                            // A small badge keeps the two kinds apart when a
                            // command and a note share a name.
                            Text {
                                Layout.preferredWidth: paletteRoot.scaled(36)
                                text: row.modelData.kind === "command" ? "CMD" : "FILE"
                                color: paletteRoot.mutedColor
                                font.family: paletteRoot.fontFamily
                                font.pixelSize: paletteRoot.scaled(10)
                                font.letterSpacing: 1
                            }

                            Text {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: row.modelData.label
                                color: paletteRoot.textColor
                                font.family: paletteRoot.fontFamily
                                font.pixelSize: paletteRoot.scaled(15)
                                elide: Text.ElideRight
                            }

                            Text {
                                visible: row.modelData.detail !== ""
                                Layout.maximumWidth: panel.width * 0.4
                                text: row.modelData.detail
                                color: paletteRoot.mutedColor
                                font.family: paletteRoot.fontFamily
                                font.pixelSize: paletteRoot.scaled(11)
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: list.currentIndex = row.index
                            onClicked: {
                                list.currentIndex = row.index;
                                paletteRoot.activateCurrent();
                            }
                        }
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: paletteRoot.scaled(18)
                    visible: paletteRoot.rows.length === 0
                    text: "No matches"
                    color: paletteRoot.mutedColor
                    font.family: paletteRoot.fontFamily
                    font.pixelSize: paletteRoot.scaled(13)
                }
            }
        }
    }
}
