pragma Singleton

import QtQuick

QtObject {
    readonly property string os: Qt.platform.os
    readonly property bool isAndroid: os === "android"
    readonly property bool isDesktop: os === "linux" || os === "windows" || os === "osx"
    readonly property bool isMobile: isAndroid

    readonly property bool useFullScreenWindow: false
    readonly property bool useMaximizedWindow: isMobile
    readonly property bool showWindowControls: isDesktop
    readonly property bool useCustomWindowFrame: isDesktop
    readonly property bool showResizeHandles: isDesktop
}