import QtQuick

// A small clickable label for the file viewer header: the Links/Folders
// switch and the collapse button. Colors and sizes come in as properties
// because a separate file cannot see the window's ids.
Text {
    id: control

    property string label
    property bool active: false
    property color activeColor: "#eeeeee"
    property color inactiveColor: "#909191"
    property string fontFamily: "IBM Plex Mono"
    property real pixelSize: 13

    signal clicked()

    text: control.label
    color: control.active ? control.activeColor : control.inactiveColor
    font.family: control.fontFamily
    font.pixelSize: control.pixelSize
    font.bold: control.active

    MouseArea {
        anchors.fill: parent
        anchors.margins: -6
        cursorShape: Qt.PointingHandCursor
        onClicked: control.clicked()
    }
}
