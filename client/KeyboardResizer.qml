import QtQuick 2.15
import QtQuick.Window 2.15

Connections {
    id: root
    target: Qt.inputMethod
    
    property int fullWindowHeight: 0
    property var targetWindow: mainWindow
    
    // Сигнал об изменении размера окна
    signal windowResized(int newHeight)
    
    // Следим за изменением области клавиатуры
    function onKeyboardRectangleChanged() {
        if (!targetWindow || !targetWindow.visible)
            return
            
        var keyboardRect = Qt.inputMethod.keyboardRectangle
        var isKeyboardVisible = keyboardRect.height > 0 && keyboardRect.y > 0
        
        if (isKeyboardVisible) {
            // Сохраняем полную высоту при первом появлении
            if (fullWindowHeight === 0)
                fullWindowHeight = targetWindow.height
                
            // Вычисляем доступную высоту (до верхней границы клавиатуры)
            var availableHeight = keyboardRect.y / Screen.devicePixelRatio
            if (availableHeight > 0 && availableHeight < fullWindowHeight) {
                targetWindow.height = availableHeight
                windowResized(availableHeight)
            }
        } else {
            // Восстанавливаем полную высоту
            if (fullWindowHeight > 0) {
                targetWindow.height = fullWindowHeight
                windowResized(fullWindowHeight)
            }
        }
    }
    
    // При уничтожении компонента восстанавливаем высоту
    Component.onDestruction: {
        if (fullWindowHeight > 0 && targetWindow)
            targetWindow.height = fullWindowHeight
    }
    
    // При активации окна сохраняем его высоту
    function init() {
        if (targetWindow) {
            fullWindowHeight = targetWindow.height
            // Принудительно проверяем состояние клавиатуры
            onKeyboardRectangleChanged()
        }
    }
}