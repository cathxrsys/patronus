import QtQuick
import QtQuick.Controls.Material
import QtQuick.Effects

Rectangle {
    id: root

    ThemeDark {
        id: theme
    }

    property string donateText: ""
    property string donateAddress: ""
    property string donateIcon: ""
    property real iconSize: 18
    property color accentColor: theme.accent
    property color backgroundColor: theme.background

    width: parent.width
    height: 30
    color: 'transparent'
    radius: 0

    anchors.horizontalCenter: parent.horizontalCenter

    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        
        anchors.leftMargin: Platform.isMobile ? 0 : 15
        anchors.rightMargin: Platform.isMobile ? 0 : 15
        spacing: 5

        ColorImage {
            id: iconImage
            source: root.donateIcon
            anchors.verticalCenter: parent.verticalCenter
            width: root.iconSize
            height: root.iconSize
            accentColor: root.accentColor
            anchors.leftMargin: 10
        }

        Text {
            text: root.donateText
            color: root.accentColor
            font.family: geologicaFont.name
            font.pixelSize: Platform.isMobile ? 12 : 14
            font.weight: Font.Bold
            width: Platform.isMobile ? 80 : 120
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 5
        }

        TextField {
            id: addressArea
            text: root.donateAddress
            width: parent.width * 0.6
            readOnly: true
            selectByMouse: true
            color: root.accentColor
            font.family: geologicaFont.name
            font.pixelSize: 12
            background: null
            verticalAlignment: Text.AlignVCenter
            anchors.verticalCenter: parent.verticalCenter
            
            leftPadding: 0
            rightPadding: 10

            selectedTextColor: root.backgroundColor

            anchors.rightMargin: 10

            onTextChanged: {
                cursorPosition = 0
            }

            Component.onCompleted: {
                cursorPosition = 0
            }
            
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                cursorShape: Qt.IBeamCursor
                onClicked: {
                    optionsMenu.popup()
                }
            }

            Menu {
                id: optionsMenu
                y: parent.height

                topInset: 0
                bottomInset: 0
                padding: 0

                property bool leftAlign: true
                property bool isRed: false

                background: Rectangle {
                    implicitWidth: parent.width
                    color: root.backgroundColor
                    border.color: theme.lowestAccent
                    radius: 4
                }

                property string addressToCopy: addressArea.text

                MyMenuItem {
                    text: qsTr("Copy to clipboard")
                    onTriggered: {
                        let savedAddress = root.donateAddress
                        clipboardHelper.setText(savedAddress)
                        addressArea.text = qsTr("Copied to clipboard")

                        let timer = Qt.createQmlObject("import QtQuick 2.0; Timer { interval: 1000; repeat: false; }", root)
                        timer.triggered.connect(() => {
                            addressArea.text = savedAddress
                            addressArea.cursorPosition = 0
                            timer.destroy()
                        })
                        timer.start()
                    }

                    leftAlign: optionsMenu.leftAlign
                    isRed: false
                    width: 200

                    colorAccent: root.accentColor
                    colorBackground: root.backgroundColor
                    colorHighlighted: "#07000000"
                }


            }
        }

        ColorImage {
            id: copyIcon
            source: "resources/copy.svg"
            anchors.verticalCenter: parent.verticalCenter
            width: 18
            height: 18
            accentColor: root.accentColor

            MouseArea {
                id: copyMouseArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    let savedAddress = root.donateAddress
                    clipboardHelper.setText(savedAddress)
                    addressArea.text = qsTr("Copied to clipboard")

                    let timer = Qt.createQmlObject("import QtQuick 2.0; Timer { interval: 1000; repeat: false; }", root)
                    timer.triggered.connect(() => {
                        addressArea.text = savedAddress
                        addressArea.cursorPosition = 0
                        timer.destroy()
                    })
                    timer.start()
                }
            }

        }
    }
}