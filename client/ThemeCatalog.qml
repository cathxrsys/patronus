import QtQuick

QtObject {
    // key -> theme file
    readonly property var sources: ({
        "dark": "ThemeTelegraph.qml",
        "nord": "ThemeNord.qml",
        "violet": "ThemeViolet.qml",
        "highContrast": "ThemeHighContrast.qml",
        "amber": "ThemeAmber.qml",
        "ocean": "ThemeOcean.qml",
        "rose": "ThemeRose.qml",
        "terminal": "ThemeDark.qml",
        "night": "ThemeNight.qml",
        "graphite": "ThemeGraphite.qml",
        "indigo": "ThemeIndigo.qml",
        "coral": "ThemeCoral.qml",
        "sage": "ThemeSage.qml",
        "copper": "ThemeCopper.qml"
    })

    function hasTheme(key) {
        return sources[key] !== undefined
    }

    function sourceFor(key) {
        return hasTheme(key) ? sources[key] : sources["dark"]
    }

    readonly property string defaultTheme: "dark"
}