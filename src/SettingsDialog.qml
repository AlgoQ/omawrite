import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The settings modal, opened from the footer's gear button: scroll speed and
// the rebindable keys. Edits apply through the backend and persist across
// windows; the shortcuts reference reads the same registry.
Dialog {
    id: root
    objectName: "settingsDialog"

    property bool darkMode: true
    property color textColor: darkMode ? "#d0d0d0" : "#42464c"
    property color strongTextColor: darkMode ? "#eeeeee" : "#222324"
    property color activeButtonColor: "#428bca"
    property real textScale: 1
    property int containerWidth: 1280
    property int containerHeight: 820
    property var rows: []
    property string errorText: ""

    title: "Settings"
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    implicitWidth: Math.round(640 * textScale)
    implicitHeight: header.implicitHeight + footer.implicitHeight
        + topPadding + bottomPadding + settingsContent.implicitHeight
    width: Math.max(0, Math.min(implicitWidth, containerWidth - 32))
    height: Math.max(0, Math.min(implicitHeight, containerHeight - 32))
    x: Math.round((containerWidth - width) / 2)
    y: Math.round((containerHeight - height) / 2)
    padding: 24

    function refresh() {
        rows = backend.shortcutRows();
        speedField.text = Number(backend.scrollSpeed).toFixed(1);
        errorText = "";
    }

    onOpened: refresh()

    Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, 0.65) }

    background: Rectangle {
        color: root.darkMode ? "#181818" : "#fbfbfb"
        border.color: root.darkMode ? "#343434" : "#d8d8d8"
        border.width: 1
        radius: 0
    }

    header: ColumnLayout {
        spacing: 6
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            Layout.topMargin: 20
            Layout.bottomMargin: 8
            text: root.title
            color: root.strongTextColor
            font.family: "IBM Plex Mono"
            font.pixelSize: Math.round(15 * root.textScale)
            font.bold: true
        }
    }

    contentItem: ScrollView {
        id: settingsScroll
        clip: true
        contentWidth: availableWidth
        contentHeight: settingsContent.implicitHeight
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            id: settingsContent
            width: settingsScroll.availableWidth
            spacing: 10

            Label {
                text: "Scrolling"
                color: root.strongTextColor
                font.family: "IBM Plex Mono"
                font.pixelSize: Math.round(13 * root.textScale)
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Label {
                    Layout.fillWidth: true
                    text: "Scroll speed (editor and file viewer)"
                    color: root.textColor
                    font.family: "IBM Plex Mono"
                    font.pixelSize: Math.round(12 * root.textScale)
                    wrapMode: Text.Wrap
                }
                TextField {
                    id: speedField
                    objectName: "scrollSpeedField"
                    Layout.preferredWidth: Math.round(64 * root.textScale)
                    text: "2.0"
                    color: root.strongTextColor
                    selectionColor: root.activeButtonColor
                    font.family: "IBM Plex Mono"
                    font.pixelSize: Math.round(12 * root.textScale)
                    onEditingFinished: {
                        var value = parseFloat(text.replace(",", "."));
                        if (isNaN(value)) {
                            root.errorText = "Scroll speed needs a number, e.g. 2.0.";
                            root.refresh();
                            return;
                        }
                        backend.scrollSpeed = value;
                        root.refresh();
                    }
                }
            }

            Label {
                Layout.topMargin: 8
                text: "Keyboard"
                color: root.strongTextColor
                font.family: "IBM Plex Mono"
                font.pixelSize: Math.round(13 * root.textScale)
                font.bold: true
            }

            Label {
                text: "Separate alternates with commas, e.g. Ctrl+Shift+Z, Ctrl+Y. A change applies immediately."
                color: root.textColor
                opacity: 0.75
                font.family: "IBM Plex Mono"
                font.pixelSize: Math.round(11 * root.textScale)
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            Repeater {
                model: root.rows
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: modelData.label
                        color: modelData.isDefault ? root.textColor : root.strongTextColor
                        font.family: "IBM Plex Mono"
                        font.pixelSize: Math.round(12 * root.textScale)
                        wrapMode: Text.Wrap
                    }
                    TextField {
                        Layout.preferredWidth: Math.min(Math.round(190 * root.textScale),
                            (settingsContent.width - 40) * 0.5)
                        text: modelData.sequence
                        color: root.strongTextColor
                        selectionColor: root.activeButtonColor
                        font.family: "IBM Plex Mono"
                        font.pixelSize: Math.round(12 * root.textScale)
                        onEditingFinished: {
                            if (text === modelData.sequence)
                                return;
                            if (!backend.setShortcut(modelData.id, text))
                                root.errorText = "«" + text + "» is not a valid key.";
                            root.refresh();
                        }
                    }
                    Text {
                        text: "↺"
                        color: modelData.isDefault ? root.textColor : root.activeButtonColor
                        opacity: modelData.isDefault ? 0.4 : 1.0
                        font.pixelSize: Math.round(14 * root.textScale)
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -8
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                backend.resetShortcut(modelData.id);
                                root.refresh();
                            }
                        }
                    }
                }
            }

            Label {
                visible: root.errorText !== ""
                text: root.errorText
                color: "#d06060"
                font.family: "IBM Plex Mono"
                font.pixelSize: Math.round(12 * root.textScale)
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }
    }

    footer: Item {
        implicitHeight: Math.max(closeButton.implicitHeight, resetButton.implicitHeight) + 20
        SquareDialogButton {
            id: resetButton
            anchors.left: parent.left
            anchors.leftMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            text: "Reset all keys"
            darkMode: root.darkMode
            textScale: root.textScale
            activeColor: root.activeButtonColor
            onClicked: {
                backend.resetAllShortcuts();
                root.refresh();
            }
        }
        SquareDialogButton {
            id: closeButton
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            text: "Close"
            primary: true
            darkMode: root.darkMode
            textScale: root.textScale
            activeColor: root.activeButtonColor
            onClicked: root.close()
        }
    }
}
