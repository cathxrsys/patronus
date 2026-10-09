import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls.Material
import QtQuick.Effects
import QtQuick.Shapes

Rectangle {
    id: root

    FontLoader {
        id: geologicaFont
        source: "resources/fonts/geologica.ttf"
    }

    width: row.implicitWidth + 16
    height: 30
    radius: 5
    color: "transparent"

    // Optional override for the back gesture. When unset the button just pops
    // the stack; a page with unsaved changes (e.g. Settings) can assign a
    // function here to intercept and, say, confirm before leaving.
    property var backAction: null

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6

        ColorImage {
            anchors.verticalCenter: parent.verticalCenter
            source: "resources/arrow_left.svg"
            width: 20
            height: 20
            accentColor: theme.accent
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Back")
            color: theme.accent
            font.pixelSize: 14
            font.family: geologicaFont.name
            font.weight: Font.Medium
            leftPadding: 5
        }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        onClicked: root.backAction ? root.backAction() : stack.pop()
    }
}

// BottomButton {
//     id: root

//     width: 30
//     height: width

//     icon.source: "resources/arrow_left.svg"
//     icon.color: theme.accent

//     icon.width: 20
//     icon.height: 20

//     property int rotationAngle: 0

//     onClicked: {
//         rotateAnim.start()
//         var timer = Qt.createQmlObject('import QtQuick 2.0; Timer { interval: 150; running: true; repeat: false; onTriggered: { stack.pop(); destroy(); } }', mainWindow);
//     }

//     rotation: rotationAngle

//     PropertyAnimation {
//         id: rotateAnim
//         target: root
//         property: "rotation"
//         from: 0
//         to: -360
//         duration: 250
//         running: false
//         onStopped: root.rotation = 0
//     }
// }
