import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

Button {
    anchors.leftMargin: 0
    anchors.rightMargin: 0
    anchors.topMargin: 0
    anchors.bottomMargin: 0

    Layout.fillHeight: true
    height: parent.height

    Layout.preferredWidth: parent.height
    Layout.minimumWidth: parent.height
    Layout.maximumWidth: parent.height

    Layout.margins: 0
    Layout.bottomMargin: 0
    Layout.topMargin: 0
    Layout.rightMargin: 0
    Layout.leftMargin: 0

    topInset: 0
    bottomInset: 0
    leftInset: 0
    rightInset: 0
    
    flat: true

    spacing: 0
    padding: 0

    icon.width: 10
    icon.height: 10

    Material.background: "transparent"
    
    Material.accent: "transparent"
    Material.primary: "transparent"

    Material.roundedScale: Material.NotRounded

    background: Rectangle {
        color: "transparent"
        radius: 0
    }

    leftPadding: 0
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0

    smooth: true
    antialiasing: true

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }
}