import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material
import QtQuick.Effects

Button {
    id: root

    property color buttonColor: Material.color(Material.Accent)
    property color backgroundColor: Material.color(Material.Background)
    property color currentIconColor: buttonColor
    property color currentTextColor: buttonColor
    property color currentBackgroundColor: backgroundColor

    text: "button"
    font.pixelSize: 12
    font.weight: Font.Medium

    icon.width: 18
    icon.height: 18

    icon.color: buttonColor

    anchors.leftMargin: 0
    anchors.rightMargin: 0
    anchors.topMargin: 0
    anchors.bottomMargin: 0

    hoverEnabled: !Platform.isMobile

    flat: true

    Material.roundedScale: Material.NotRounded

    focusPolicy: Platform.isMobile ? Qt.NoFocus : Qt.StrongFocus

    function isHoverActive() {
        return !Platform.isMobile && hoverHandler.hovered
    }

    function resetVisualState() {
        root.currentIconColor = root.buttonColor
        root.currentTextColor = root.buttonColor
        root.currentBackgroundColor = root.backgroundColor
    }

    background: Rectangle {
        color: root.currentBackgroundColor
        radius: 0
    }

    // HoverHandler {
        
    // }

    Layout.preferredHeight: 60
    Layout.minimumHeight: 60
    Layout.maximumHeight: 60
    Layout.margins: 0

    leftInset: 0
    rightInset: 0
    topInset: 0
    bottomInset: 0
    

    contentItem: RowLayout {
        spacing: 0

        Item {
            Layout.preferredWidth: root.icon.width
            Layout.preferredHeight: root.icon.height
            Layout.alignment: Qt.AlignVCenter
            Layout.rightMargin: 10

            ColorImage {
                id: iconImageColored
                anchors.centerIn: parent
                width: root.icon.width
                height: root.icon.height
                source: root.icon.source
                accentColor: root.currentIconColor
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            Text {
                id: textMeasure
                text: root.text
                font: root.font
                visible: false
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                width: parent.width
                
            }

            Text {
                text: {
                    let lines = root.text.split('\n')
                    return lines.length > 0 ? lines[0] : root.text
                }
                font: root.font
                color: root.currentTextColor
                horizontalAlignment: Text.AlignLeft
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                Layout.fillWidth: true
                visible: textMeasure.lineCount > 0
            }

            Text {
                text: {
                    let lines = root.text.split('\n')
                    return lines.length > 1 ? lines.slice(1).join('\n') : ''
                }
                font: root.font
                color: root.currentTextColor
                horizontalAlignment: Text.AlignLeft
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideMiddle
                Layout.fillWidth: true
                visible: textMeasure.lineCount > 1
            }
        }
    }

    HoverHandler {
        id: hoverHandler
        enabled: !Platform.isMobile
        cursorShape: Qt.PointingHandCursor
        onHoveredChanged: {
            if (hovered) {
                root.currentIconColor = root.backgroundColor
                root.currentTextColor = root.backgroundColor
                root.currentBackgroundColor = root.buttonColor
            } else {
                root.resetVisualState()
            }
        }
    }

    onPressedChanged: {
        if (!pressed && Platform.isMobile)
            resetVisualState()
    }

    onButtonColorChanged: {
        if (!pressed && !isHoverActive())
            resetVisualState()
    }

    onBackgroundColorChanged: {
        if (!pressed && !isHoverActive())
            resetVisualState()
    }

    Component.onCompleted: resetVisualState()
}