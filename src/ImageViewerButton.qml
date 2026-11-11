import QtQuick
import QtQuick.Controls
import QtQuick.Window

// A mostly transparent mark over the full-window image viewer: a chevron at
// the picture's side, or the close mark. No plate or background, so it takes
// as little of the picture as possible until the pointer finds it.
Item {
    id: button

    // -1 points left, 1 points right; closeMark draws an X instead.
    property int direction: 1
    property bool closeMark: false

    signal clicked()

    opacity: hoverArea.containsMouse ? 0.95 : 0.42
    Behavior on opacity {
        NumberAnimation { duration: 100 }
    }

    Canvas {
        id: iconCanvas

        // Canvas rasterizes one surface pixel per logical pixel and gets
        // upscaled blurry on hidpi screens. Draw at the physical size and
        // scale the item back down so surface pixels match screen pixels.
        readonly property real dpr: Screen.devicePixelRatio
        width: button.width * dpr
        height: button.height * dpr
        transformOrigin: Item.TopLeft
        scale: 1 / dpr
        onDprChanged: requestPaint()

        onPaint: {
            var context = getContext("2d");
            context.setTransform(dpr, 0, 0, dpr, 0, 0);
            context.clearRect(0, 0, width, height);
            context.lineCap = "round";
            context.lineJoin = "round";

            var stroke = function(color, lineWidth) {
                context.strokeStyle = color;
                context.lineWidth = lineWidth;
                context.beginPath();
                var centerX = button.width / 2;
                var centerY = button.height / 2;
                if (button.closeMark) {
                    var arm = Math.min(button.width, button.height) * 0.18;
                    context.moveTo(centerX - arm, centerY - arm);
                    context.lineTo(centerX + arm, centerY + arm);
                    context.moveTo(centerX + arm, centerY - arm);
                    context.lineTo(centerX - arm, centerY + arm);
                } else {
                    var reach = Math.min(button.height * 0.16, 20);
                    var depth = reach * 0.62;
                    var tip = centerX + button.direction * depth;
                    var tail = centerX - button.direction * depth;
                    context.moveTo(tail, centerY - reach);
                    context.lineTo(tip, centerY);
                    context.lineTo(tail, centerY + reach);
                }
                context.stroke();
            };

            // A dark halo keeps the mark readable over a pale picture.
            stroke("rgba(0, 0, 0, 0.45)", 7);
            stroke("#f4f4f4", 2.6);
        }
    }

    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
