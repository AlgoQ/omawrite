import QtQuick
import QtQuick.Controls
import QtQuick.Window

// A full-window viewer for the pictures embedded in the open note. The
// picture is shown at its own size up to the window's, and the chevrons move
// through the note's other pictures: at the edges of this one when it leaves
// room, over its edge when it does not. Left and Right do the same from the
// keyboard, Escape closes.
Item {
    id: viewer

    // The image maps from Backend::imageEmbeds, in document order.
    property var images: []
    property int index: -1
    property string fontFamily
    property real textScale: 1.0
    property int margin: 56

    signal stepRequested(int direction)
    signal closeRequested()

    readonly property var current:
        index >= 0 && index < images.length ? images[index] : null
    readonly property bool hasPrevious: index > 0
    readonly property bool hasNext: index >= 0 && index < images.length - 1

    // The name and count keep their room even when the desktop's text size
    // is large, so a tall picture cannot grow over them.
    readonly property int labelHeight: Math.max(16, Math.round(13 * textScale) + 6)
    readonly property int bottomMargin: Math.max(margin, labelHeight + 24)

    // Never enlarged beyond its own pixels, so a small picture stays crisp
    // and a large one shrinks to fit the window.
    readonly property size pictureSize: {
        if (!current)
            return Qt.size(0, 0);
        var naturalWidth = current.naturalWidth > 0 ? current.naturalWidth : current.imageWidth;
        var naturalHeight = current.naturalHeight > 0 ? current.naturalHeight : current.imageHeight;
        if (!naturalWidth || !naturalHeight)
            return Qt.size(0, 0);
        var maxWidth = Math.max(1, width - 2 * margin);
        var maxHeight = Math.max(1, height - margin - bottomMargin);
        var scale = Math.min(maxWidth / naturalWidth, maxHeight / naturalHeight, 1);
        return Qt.size(Math.round(naturalWidth * scale), Math.round(naturalHeight * scale));
    }
    readonly property rect pictureRect: {
        var fitted = pictureSize;
        var top = margin;
        var bottom = height - bottomMargin;
        return Qt.rect(Math.round((width - fitted.width) / 2),
                       Math.round(top + (bottom - top - fitted.height) / 2),
                       fitted.width, fitted.height);
    }

    visible: opacity > 0
    opacity: 0
    z: 200

    Behavior on opacity {
        NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
    }

    function open() {
        opacity = 1;
        forceActiveFocus();
    }

    function close() {
        opacity = 0;
    }

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Left) {
            viewer.stepRequested(-1);
            event.accepted = true;
        } else if (event.key === Qt.Key_Right) {
            viewer.stepRequested(1);
            event.accepted = true;
        } else if (event.key === Qt.Key_Escape) {
            viewer.closeRequested();
            event.accepted = true;
        }
    }

    Rectangle {
        anchors.fill: parent
        // Fully opaque: the note behind a screenshot should not ghost through.
        color: "#0b0b0b"

        // The dark space around the picture closes the viewer.
        MouseArea {
            anchors.fill: parent
            onClicked: viewer.closeRequested()
        }
    }

    Image {
        id: picture
        objectName: "imageViewerPicture"
        x: viewer.pictureRect.x
        y: viewer.pictureRect.y
        width: viewer.pictureRect.width
        height: viewer.pictureRect.height
        source: viewer.current ? viewer.current.imageUrl : ""
        // Decode for the pixels actually drawn; a camera-sized file need not
        // arrive at more than the window can show.
        sourceSize.width: Math.round(width * Screen.devicePixelRatio)
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        smooth: true
        mipmap: true
    }

    // A press on the picture itself inspects it, it does not dismiss it.
    MouseArea {
        x: picture.x
        y: picture.y
        width: picture.width
        height: picture.height
        acceptedButtons: Qt.AllButtons
    }

    ImageViewerButton {
        objectName: "imageViewerPrevious"
        direction: -1
        width: 56
        height: 120
        visible: viewer.hasPrevious
        x: Math.max(8, viewer.pictureRect.x - width - 8)
        y: Math.round((viewer.height - height) / 2)
        onClicked: viewer.stepRequested(-1)
    }

    ImageViewerButton {
        objectName: "imageViewerNext"
        direction: 1
        width: 56
        height: 120
        visible: viewer.hasNext
        x: Math.min(viewer.width - width - 8,
                    viewer.pictureRect.x + viewer.pictureRect.width + 8)
        y: Math.round((viewer.height - height) / 2)
        onClicked: viewer.stepRequested(1)
    }

    ImageViewerButton {
        objectName: "imageViewerClose"
        closeMark: true
        width: 44
        height: 44
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 10
        onClicked: viewer.closeRequested()
    }

    Label {
        objectName: "imageViewerLabel"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 18
        width: Math.min(implicitWidth, viewer.width - 160)
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideMiddle
        text: {
            if (!viewer.current)
                return "";
            var name = viewer.current.imageName
                ? viewer.current.imageName : viewer.current.imageUrl.toString();
            return viewer.images.length > 1
                ? name + "  ·  " + (viewer.index + 1) + " / " + viewer.images.length
                : name;
        }
        color: "#ffffff"
        opacity: 0.6
        font.family: viewer.fontFamily
        font.pixelSize: Math.max(11, Math.round(13 * viewer.textScale))
    }
}
