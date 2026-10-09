import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

Item {
    id: root

    FontLoader {
        id: geologicaFont
        source: "resources/fonts/geologica.ttf"
    }
    ThemeDark {
        id: theme
    }

    property bool transparentBool: false
    property color backgroundColor: theme.background
    property color accentColor: theme.accent
    property color lowestAccentColor: theme.minimumAccent   

    // Elevated surface + rim so the language dropdown matches the app's panels.
    property color surfaceColor: backgroundColor
    property color borderColor: lowestAccentColor

    implicitWidth: 140
    implicitHeight: 50

    Rectangle {
        id: languageChangeButton
        anchors.fill: parent

        color: root.backgroundColor
        border.color: root.transparentBool ? "transparent" : root.accentColor
        border.width: 1
        radius: 0

        Row {
            anchors.centerIn: parent
            spacing: 15

            Image {
                source: "resources/flags/" + languageChanger.currentFlagAssetCode + ".svg"
                width: 24
                height: 24
                mipmap: true
                smooth: true
                antialiasing: true
            }

            Text {
                text: languageChanger.currentLocaleFullName
                color: root.accentColor
                font.pixelSize: 14
                font.family: geologicaFont.name
                verticalAlignment: Text.AlignVCenter
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                languagesMenu.open()
            }
        }
    }

    Menu {
        id: languagesMenu
        y: languageChangeButton.y + languageChangeButton.height
        x: languageChangeButton.x + (languageChangeButton.width - languagesMenu.width) / 2
        topInset: 0
        bottomInset: 0
        padding: 0
        property bool leftAlign: true
        property bool isRed: false
        background: Rectangle {
            implicitWidth: 125
            color: root.surfaceColor
            border.color: root.borderColor
            border.width: 1
            radius: 14

            PanelBorder {
                edgeColor: root.borderColor
            }
        }
        MyMenuItemWithIcon {
            text: "English"
            onTriggered: {
                languageChanger.changeLanguage(0)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/en.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Русский"
            onTriggered: {
                languageChanger.changeLanguage(1)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/ru.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Українська"
            onTriggered: {
                languageChanger.changeLanguage(2)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/ua.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Қазақша"
            onTriggered: {
                languageChanger.changeLanguage(3)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/kz.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Polski"
            onTriggered: {
                languageChanger.changeLanguage(4)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/pl.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Français"
            onTriggered: {
                languageChanger.changeLanguage(5)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/fr.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Deutsch"
            onTriggered: {
                languageChanger.changeLanguage(6)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/de.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Italiano"
            onTriggered: {
                languageChanger.changeLanguage(7)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/it.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Español"
            onTriggered: {
                languageChanger.changeLanguage(8)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/es.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Nederlands"
            onTriggered: {
                languageChanger.changeLanguage(9)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/nl.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "中文"
            onTriggered: {
                languageChanger.changeLanguage(10)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/cn.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Беларуская"
            onTriggered: {
                languageChanger.changeLanguage(11)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/by.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Suomi"
            onTriggered: {
                languageChanger.changeLanguage(12)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/fi.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Svenska"
            onTriggered: {
                languageChanger.changeLanguage(13)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/se.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Română"
            onTriggered: {
                languageChanger.changeLanguage(14)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/ro.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
        MyMenuItemWithIcon {
            text: "Тоҷикӣ"
            onTriggered: {
                languageChanger.changeLanguage(15)
            }
            leftAlign: languagesMenu.leftAlign
            isRed: false
            width: 125
            iconsource: "resources/flags/tj.svg"
            backgroundColor: root.backgroundColor
            accentColor: root.accentColor
            lowestAccentColor: root.lowestAccentColor
        }
    }
}