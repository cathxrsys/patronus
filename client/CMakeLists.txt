cmake_minimum_required(VERSION 3.16)

project(client VERSION 1.0.0 LANGUAGES C CXX)

if(WIN32)
    set(PATRONUS_WINDOWS_LIB_CONFIG "Release" CACHE STRING "Windows configuration used for internal prebuilt libraries")
endif()

option(BUILD_APPIMAGE "Build as portable AppImage bundle" OFF)
option(BUILD_WINDOWS_PORTABLE "Build as portable Windows executable (single exe with embedded dependencies)" OFF)

if(BUILD_APPIMAGE AND NOT ANDROID)
    message(STATUS "🔧 Building for AppImage (portable bundle)")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(ANDROID)
    set(ANDROID_DEPS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/android/${ANDROID_ABI})
    set(ANDROID_INCLUDE_DIR ${ANDROID_DEPS_ROOT}/include)
    set(ANDROID_LIB_DIR ${ANDROID_DEPS_ROOT}/lib)

    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/opus/opus.h)
        message(FATAL_ERROR "Android opus headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_LIB_DIR}/libopus.a)
        message(FATAL_ERROR "Android opus library not found in ${ANDROID_LIB_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/sodium.h)
        message(FATAL_ERROR "Android sodium headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_LIB_DIR}/libsodium.a)
        message(FATAL_ERROR "Android sodium library not found in ${ANDROID_LIB_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/spdlog/spdlog.h)
        message(FATAL_ERROR "Android spdlog headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_LIB_DIR}/libspdlog.a)
        message(FATAL_ERROR "Android spdlog library not found in ${ANDROID_LIB_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/fmt/format.h)
        message(FATAL_ERROR "Android fmt headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_LIB_DIR}/libfmt.a)
        message(FATAL_ERROR "Android fmt library not found in ${ANDROID_LIB_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/nlohmann/json.hpp)
        message(FATAL_ERROR "Android nlohmann headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/openssl/ssl.h)
        message(FATAL_ERROR "Android OpenSSL headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_LIB_DIR}/libssl.a)
        message(FATAL_ERROR "Android OpenSSL SSL library not found in ${ANDROID_LIB_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_LIB_DIR}/libcrypto.a)
        message(FATAL_ERROR "Android OpenSSL crypto library not found in ${ANDROID_LIB_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_INCLUDE_DIR}/oqs/oqs.h)
        message(FATAL_ERROR "Android liboqs headers not found in ${ANDROID_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS ${ANDROID_LIB_DIR}/liboqs.a)
        message(FATAL_ERROR "Android liboqs library not found in ${ANDROID_LIB_DIR}")
    endif()

    set(OPUS_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(OPUS_LIBRARY ${ANDROID_LIB_DIR}/libopus.a)
    set(SODIUM_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(SODIUM_LIBRARY ${ANDROID_LIB_DIR}/libsodium.a)
    set(SPDLOG_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(SPDLOG_LIBRARY ${ANDROID_LIB_DIR}/libspdlog.a)
    set(FMT_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(FMT_LIBRARY ${ANDROID_LIB_DIR}/libfmt.a)
    set(NLOHMANN_JSON_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(OPENSSL_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(OPENSSL_SSL_LIBRARY ${ANDROID_LIB_DIR}/libssl.a)
    set(OPENSSL_CRYPTO_LIBRARY ${ANDROID_LIB_DIR}/libcrypto.a)
    set(ANDROID_OPENSSL_RUNTIME_DIR ${ANDROID_LIB_DIR}/runtime)
    set(ANDROID_OPENSSL_SSL_RUNTIME_LIBRARY ${ANDROID_OPENSSL_RUNTIME_DIR}/libssl_3.so)
    set(ANDROID_OPENSSL_CRYPTO_RUNTIME_LIBRARY ${ANDROID_OPENSSL_RUNTIME_DIR}/libcrypto_3.so)
    set(LIBOQS_INCLUDE_DIR ${ANDROID_INCLUDE_DIR})
    set(LIBOQS_LIBRARY ${ANDROID_LIB_DIR}/liboqs.a)

    add_subdirectory(../corecrypto ${CMAKE_CURRENT_BINARY_DIR}/corecrypto)
    add_subdirectory(../doubleratchet ${CMAKE_CURRENT_BINARY_DIR}/doubleratchet)

    set(PATRONUS_THIRD_PARTY_INCLUDE_DIRS
        ${LIBOQS_INCLUDE_DIR}
        ${OPUS_INCLUDE_DIR}
        ${SODIUM_INCLUDE_DIR}
        ${SPDLOG_INCLUDE_DIR}
        ${FMT_INCLUDE_DIR}
        ${NLOHMANN_JSON_INCLUDE_DIR}
        ${OPENSSL_INCLUDE_DIR}
    )

    set(PATRONUS_INTERNAL_LIBS
        corecrypto
        doubleratchet
    )

    set(PATRONUS_THIRD_PARTY_LIBS
        ${SODIUM_LIBRARY}
        ${SPDLOG_LIBRARY}
        ${FMT_LIBRARY}
        ${LIBOQS_LIBRARY}
        ${OPENSSL_CRYPTO_LIBRARY}
        ${OPUS_LIBRARY}
    )
else()
    # Windows / Linux / macOS
    if(WIN32)
        message(STATUS "🔧 Building for Windows")
        
        # Use VCPKG_PREFIX_PATH if it is set (for static builds)
        if(DEFINED VCPKG_PREFIX_PATH)
            set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${VCPKG_PREFIX_PATH})
        endif()
        
        # On Windows use find_library instead of pkg-config
        find_path(OPUS_INCLUDE_DIR opus/opus.h)
        find_library(OPUS_LIBRARY NAMES opus)
        
        find_path(SODIUM_INCLUDE_DIR sodium.h)
        find_library(SODIUM_LIBRARY NAMES sodium libsodium)
        
        find_path(SPDLOG_INCLUDE_DIR spdlog/spdlog.h)
        find_library(SPDLOG_LIBRARY NAMES spdlog)
        
        find_path(FMT_INCLUDE_DIR fmt/format.h)
        find_library(FMT_LIBRARY NAMES fmt)
        
        find_path(LIBOQS_INCLUDE_DIR oqs/oqs.h)
        find_library(LIBOQS_LIBRARY NAMES oqs)
        
        find_package(OpenSSL REQUIRED COMPONENTS Crypto)
        
        # If not found via standard paths, try vcpkg
        if(NOT SODIUM_INCLUDE_DIR AND DEFINED ENV{VCPKG_ROOT})
            set(VCPKG_BASE $ENV{VCPKG_ROOT}/installed/x64-windows-static)
            find_path(SODIUM_INCLUDE_DIR sodium.h PATHS ${VCPKG_BASE}/include REQUIRED)
            find_library(SODIUM_LIBRARY NAMES sodium libsodium PATHS ${VCPKG_BASE}/lib REQUIRED)
            find_path(OPUS_INCLUDE_DIR opus/opus.h PATHS ${VCPKG_BASE}/include REQUIRED)
            find_library(OPUS_LIBRARY NAMES opus PATHS ${VCPKG_BASE}/lib REQUIRED)
            find_path(SPDLOG_INCLUDE_DIR spdlog/spdlog.h PATHS ${VCPKG_BASE}/include REQUIRED)
            find_library(SPDLOG_LIBRARY NAMES spdlog PATHS ${VCPKG_BASE}/lib REQUIRED)
            find_path(FMT_INCLUDE_DIR fmt/format.h PATHS ${VCPKG_BASE}/include REQUIRED)
            find_library(FMT_LIBRARY NAMES fmt PATHS ${VCPKG_BASE}/lib REQUIRED)
            find_path(LIBOQS_INCLUDE_DIR oqs/oqs.h PATHS ${VCPKG_BASE}/include REQUIRED)
            find_library(LIBOQS_LIBRARY NAMES oqs PATHS ${VCPKG_BASE}/lib REQUIRED)
        endif()
        
        if(NOT SODIUM_INCLUDE_DIR)
            message(FATAL_ERROR "❌ Sodium not found. Install with: vcpkg install libsodium:x64-windows-static")
        endif()
        if(NOT OPUS_INCLUDE_DIR)
            message(FATAL_ERROR "❌ Opus not found. Install with: vcpkg install opus:x64-windows-static")
        endif()
        
        set(OPUS_INCLUDE_DIRS ${OPUS_INCLUDE_DIR})
        set(OPUS_LIBRARIES ${OPUS_LIBRARY})
        set(SODIUM_INCLUDE_DIRS ${SODIUM_INCLUDE_DIR})
        set(SODIUM_LIBRARIES ${SODIUM_LIBRARY})
        set(SPDLOG_INCLUDE_DIRS ${SPDLOG_INCLUDE_DIR})
        set(SPDLOG_LIBRARIES ${SPDLOG_LIBRARY})
        set(FMT_INCLUDE_DIRS ${FMT_INCLUDE_DIR})
        set(FMT_LIBRARIES ${FMT_LIBRARY})
        
        # Windows system libraries
        set(WINDOWS_SYSTEM_LIBS
            ws2_32 iphlpapi user32 gdi32 shell32 ole32 oleaut32
            uuid comctl32 advapi32 crypt32 secur32 netapi32 dnsapi
        )
        
        # For static linking of the C++ runtime
        if(CMAKE_BUILD_TYPE STREQUAL "Release")
            set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
        else()
            set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
        endif()
        
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(OPUS REQUIRED opus)
        pkg_check_modules(SODIUM REQUIRED libsodium)
        pkg_check_modules(SPDLOG REQUIRED spdlog)
        pkg_check_modules(FMT REQUIRED fmt)
    endif()
endif()

set(QT_CLIENT_COMPONENTS
    Core
    Gui
    Network
    Qml
    Quick
    Sql
    WebSockets
    Concurrent
    Multimedia
    LinguistTools
)

if(NOT ANDROID)
    list(APPEND QT_CLIENT_COMPONENTS Widgets Svg)
endif()

# Force the path on macOS (Apple Silicon)
if(APPLE)
    link_directories(/opt/homebrew/lib)
    include_directories(/opt/homebrew/include)
endif()

find_package(Qt6 REQUIRED COMPONENTS ${QT_CLIENT_COMPONENTS})

if(NOT ANDROID)
    if(NOT WIN32)
        find_package(OpenSSL REQUIRED COMPONENTS Crypto)
        
        find_path(LIBOQS_INCLUDE_DIR oqs/oqs.h)
        find_library(LIBOQS_LIBRARY NAMES oqs)

        if (NOT LIBOQS_INCLUDE_DIR OR NOT LIBOQS_LIBRARY)
            message(FATAL_ERROR "liboqs not found. Provide -DLIBOQS_INCLUDE_DIR and -DLIBOQS_LIBRARY.")
        endif()
    endif()

    set(PATRONUS_THIRD_PARTY_INCLUDE_DIRS
        ${LIBOQS_INCLUDE_DIR}
        ${OPUS_INCLUDE_DIRS}
        ${SODIUM_INCLUDE_DIRS}
        ${SPDLOG_INCLUDE_DIRS}
        ${FMT_INCLUDE_DIRS}
    )

    if(BUILD_APPIMAGE AND NOT WIN32)
        set(PATRONUS_INTERNAL_LIBS
            ${CMAKE_CURRENT_SOURCE_DIR}/../corecrypto/build/libcorecrypto.so
            ${CMAKE_CURRENT_SOURCE_DIR}/../doubleratchet/build/libdoubleratchet.so
        )
    elseif(WIN32)
        set(PATRONUS_INTERNAL_LIBS
            ${CMAKE_CURRENT_SOURCE_DIR}/../corecrypto/build-windows/${PATRONUS_WINDOWS_LIB_CONFIG}/corecrypto.lib
            ${CMAKE_CURRENT_SOURCE_DIR}/../doubleratchet/build-windows/${PATRONUS_WINDOWS_LIB_CONFIG}/doubleratchet.lib
        )
    else()
        set(PATRONUS_INTERNAL_LIBS
            ${CMAKE_CURRENT_SOURCE_DIR}/../corecrypto/build/libcorecrypto.a
            ${CMAKE_CURRENT_SOURCE_DIR}/../doubleratchet/build/libdoubleratchet.a
        )
    endif()

    set(PATRONUS_THIRD_PARTY_LIBS
        ${SODIUM_LIBRARIES}
        ${SPDLOG_LIBRARIES}
        ${FMT_LIBRARIES}
        ${LIBOQS_LIBRARY}
        OpenSSL::Crypto
        ${OPUS_LIBRARIES}
    )
    
    if(WIN32)
        list(APPEND PATRONUS_THIRD_PARTY_LIBS ${WINDOWS_SYSTEM_LIBS})
    endif()
endif()

qt_standard_project_setup(
    REQUIRES 6.8
    I18N_TRANSLATED_LANGUAGES ru uk de es fr it kk pl nl zh be fi sv ro tg
)

set(TS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_ru.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_uk.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_de.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_es.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_fr.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_it.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_kk.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_pl.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_nl.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_zh.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_be.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_fi.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_sv.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_ro.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/client_tg.ts
)

set_source_files_properties(Platform.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(../icons/main.png PROPERTIES QT_RESOURCE_ALIAS main.png)

qt_add_executable(patronus
    main.cpp
    src/clipboardhelper.h
    src/fonthelper.h
    src/wheelscroller.h
    src/apppaths.cpp
    src/databasecontactstore.cpp
    src/databaseschemaservice.cpp
    src/databasesettingsservice.cpp
    src/databasemessagestore.cpp
    src/databasemanager.cpp
    src/appsessionmanager.cpp
    src/corecryptoadapter.cpp
    src/appnotifier.cpp
    src/accountmanager.cpp
    src/settingsmanager.cpp
    src/audiodevicemanager.cpp
    src/audiopermission.cpp
    src/audiopermission.h
    src/connectionstorage.cpp
    src/connectiontransport.cpp
    src/connectionprotocolhandler.cpp
    src/connectionmanager.cpp
    src/connectionpayloadprocessor.cpp
    src/diagnosticsmanager.cpp
    src/namestylevalidator.cpp
    src/androidsystemui.cpp
    src/mobilepushmanager.cpp
    src/updatemanager.cpp
    src/filepicker.cpp
    src/fileopener.cpp
    src/filetransfermanager.cpp
    src/contactsmodel.cpp
    src/messagemodel.cpp
    src/callmanager.cpp
    src/voicemessagemanager.cpp
    src/voiceclipplayer.cpp
    src/voicecliprecorder.cpp
    src/voicecliputils.cpp
    src/imageprovider.h
    src/inviteutils.h
    src/onlinemanager.h
    src/qutils.h
    src/opusplayer.cpp
    src/opusrecorder.cpp
    src/mainsignals.h
    src/languagechanger.cpp
    src/mobilepushmanager.h
    src/updatemanager.h
    src/appversion.h
    src/secondtimer.h
    src/qrgenerator.cpp
    src/qrgenerator.h
    src/qrscanner.cpp
    src/qrscanner.h
    third_party/qrcodegen/qrcodegen.c
    third_party/quirc/quirc.c
    third_party/quirc/decode.c
    third_party/quirc/identify.c
    third_party/quirc/version_db.c
    ../doubleratchet/doubleratchet.cpp
    ../doubleratchet/pqdh.cpp
    ../doubleratchet/pq.h
)

qt_add_resources(patronus app_icons
    PREFIX "/icons"
    FILES
        ../icons/main.png
)

target_include_directories(patronus
    PRIVATE
        src
        third_party/qrcodegen
        third_party/quirc
        ../corecrypto
        ../doubleratchet
        ../coreutils
    ${PATRONUS_THIRD_PARTY_INCLUDE_DIRS}
)

target_compile_definitions(patronus PRIVATE
    PATRONUS_VERSION="${PROJECT_VERSION}"
    PATRONUS_PROTOCOL_VERSION="3"
)

# Windows-specific definitions
if(WIN32)
    target_compile_definitions(patronus PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )

    target_sources(patronus PRIVATE
        windows/patronus.rc
    )
endif()

set(PATRONUS_QML_MODULE_ARGS)
if(WIN32)
    list(APPEND PATRONUS_QML_MODULE_ARGS NO_GENERATE_QMLTYPES)
endif()

qt_add_qml_module(patronus
    ${PATRONUS_QML_MODULE_ARGS}
    URI client
    VERSION 1.0
    QML_FILES
        Platform.qml
        Main.qml

        KeyboardResizer.qml

        CallWindow.qml
        MobileCallView.qml

        ThemeCatalog.qml
        ThemeDark.qml
        ThemeViolet.qml
        ThemeHighContrast.qml
        ThemeAmber.qml
        ThemeOcean.qml
        ThemeRose.qml
        ThemeNord.qml
        ThemeNight.qml
        ThemeGraphite.qml
        ThemeIndigo.qml
        ThemeCoral.qml
        ThemeSage.qml
        ThemeCopper.qml
        ThemeTelegraph.qml

        PanelButton.qml
        MenuBackIcon.qml
        BottomButton.qml
        ActionButton.qml
        GradientLine.qml
        PanelBorder.qml
        BackButton.qml
        RightBarButton.qml
        BoolSetting.qml
        SettingsGroup.qml
        TextSetting.qml
        ManyTextSetting.qml
        Donate.qml
        MyMenuItem.qml
        MyMenuItemWithIcon.qml
        MyMenuItemWithColorIcon.qml
        LanguagesBox.qml
        ColorImage.qml
        Logo.qml

        StyledName.qml
        NameStyleField.qml
        qrcode.js
        NameStyleParser.js
        NameStylePresets.js
        EmojiData.js
        EmojiPicker.qml
    RESOURCES
        resources/close.svg
        resources/minimize.svg
        resources/maximize.svg
        resources/logo.svg
        resources/restore.svg
        resources/settings.svg
        resources/arrow_left.svg
        resources/arrow-left-only.svg
        resources/chats.svg
        resources/add.svg
        resources/add_contact.svg
        resources/add_chat.svg
        resources/fonts/geologica.ttf
        resources/fonts/NotoColorEmoji.ttf
        resources/smile.svg
        resources/keyboard.svg
        resources/avatars/anonymous.svg
        resources/checked.svg
        resources/double_checked.svg
        resources/call.svg
        resources/call_accent.svg
        resources/call_background.svg
        resources/send.svg
        resources/remove.svg
        resources/clear.svg
        resources/secure.svg
        resources/qr.svg
        resources/share.svg
        resources/fingerprint.svg
        resources/key.svg
        resources/server.svg
        resources/sending.svg
        resources/internet.svg
        resources/notebook.svg
        resources/blowfish.svg
        resources/arrows.svg
        resources/save.svg
        resources/loading.svg
        resources/donate.svg
        resources/copy.svg
        resources/brush.svg
        resources/timer.svg
        resources/attach.svg
        resources/photo.svg
        resources/mic.svg
        resources/play.svg
        resources/pause.svg
        resources/folder.svg
        resources/file.svg
        resources/music.svg
        resources/image.svg
        resources/video.svg
        resources/db.svg
        resources/txt.svg
        resources/xlsx.svg
        resources/docx.svg
        resources/pdf.svg
        resources/apk.svg
        resources/code.svg
        resources/cancel.svg
        resources/reply.svg
        resources/download.svg
        resources/arrow-down.svg
        resources/arrow-down-left.svg
        resources/arrow-up-right.svg
        resources/burger.svg
        resources/speaker.svg
        resources/update.svg
        resources/info.svg
        resources/undo.svg
        resources/redo.svg
        resources/selectall.svg
        resources/paste.svg
        resources/cut.svg
        resources/menu.svg
        resources/hacker.svg
        resources/github.svg

        resources/btc.svg
        resources/eth.svg
        resources/etc.svg
        resources/xmr.svg
        resources/bsc.svg
        resources/usdt.svg
        resources/doge.svg
        resources/trx.svg
        resources/ltc.svg
        resources/ton.svg
        
        resources/flags/by.svg
        resources/flags/ca.svg
        resources/flags/fi.svg
        resources/flags/nl.svg
        resources/flags/pl.svg
        resources/flags/ro.svg
        resources/flags/ru.svg
        resources/flags/sl.svg
        resources/flags/tz.svg
        resources/flags/ua.svg
        resources/flags/us.svg
        resources/flags/en.svg
        resources/flags/kz.svg
        resources/flags/de.svg
        resources/flags/fr.svg
        resources/flags/it.svg
        resources/flags/es.svg
        resources/flags/cn.svg
        resources/flags/ch.svg
        resources/flags/se.svg
        resources/flags/tj.svg

        resources/sounds/incoming_call_Default.wav
        resources/sounds/outgoing_calling.wav
        resources/sounds/call_discard.wav
        resources/sounds/accept_call.wav
        resources/sounds/notification.wav
        resources/sounds/notification_current_chat.wav
)

set_target_properties(patronus PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

if(ANDROID)
    if(NOT EXISTS ${ANDROID_OPENSSL_SSL_RUNTIME_LIBRARY})
        message(FATAL_ERROR "Android OpenSSL runtime library not found: ${ANDROID_OPENSSL_SSL_RUNTIME_LIBRARY}")
    endif()
    if(NOT EXISTS ${ANDROID_OPENSSL_CRYPTO_RUNTIME_LIBRARY})
        message(FATAL_ERROR "Android OpenSSL runtime library not found: ${ANDROID_OPENSSL_CRYPTO_RUNTIME_LIBRARY}")
    endif()

    set_target_properties(patronus PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/android
        QT_ANDROID_EXTRA_LIBS "${ANDROID_OPENSSL_CRYPTO_RUNTIME_LIBRARY};${ANDROID_OPENSSL_SSL_RUNTIME_LIBRARY}"
        # Must be non-null: Vivo's camera framework passes the package versionName
        # to ICameraService.setVersionName() when opening the camera, and a null
        # version makes openCamera throw NPE (see android/build.gradle + QrScanner).
        QT_ANDROID_VERSION_NAME "1.0.0"
        QT_ANDROID_VERSION_CODE 1
    )
endif()

target_link_libraries(patronus PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::Sql
    Qt6::WebSockets
    Qt6::Concurrent
    Qt6::Multimedia
    ${PATRONUS_INTERNAL_LIBS}
    ${PATRONUS_THIRD_PARTY_LIBS}
)

if(NOT ANDROID)
    target_link_libraries(patronus PRIVATE Qt6::Widgets Qt6::Svg)
endif()

# Force the extra image-format plugins (WebP/TIFF/ICNS) into the deployment.
# They come from the qtimageformats add-on and are NOT bundled by default:
# androiddeployqt only ships qtbase's gif/ico/jpeg + qsvg. Many senders label
# WebP files with a .png name, so without qwebp such images fail to decode and
# render blank in previews and the media viewer. qt_import_plugins feeds the
# plugin into androiddeployqt/windeployqt/linuxdeploy plugin lists. Guarded on
# TARGET so the build still works when the qtimageformats add-on is absent
# (then these formats simply stay unsupported instead of failing to configure).
foreach(_patronus_img_plugin QWebpPlugin QTiffPlugin QICNSPlugin)
    if(TARGET Qt6::${_patronus_img_plugin})
        qt_import_plugins(patronus INCLUDE Qt6::${_patronus_img_plugin})
    else()
        message(WARNING
            "Qt6::${_patronus_img_plugin} not found — install the qtimageformats "
            "add-on so WebP/TIFF images can be decoded; they will render blank "
            "without it.")
    endif()
endforeach()

include(GNUInstallDirs)
install(TARGETS patronus
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

file(GLOB TRANSLATABLE_SOURCES
    "src/*.cpp"
    "src/*.h"
    "*.cpp"
    "*.h"
    "*.qml"
)

qt_add_translations(patronus
    SOURCES ${TRANSLATABLE_SOURCES}
    TS_FILES ${TS_FILES}
    QM_FILES_OUTPUT_VARIABLE QM_FILES
)

# Important: tell CMake these are generated files
set_source_files_properties(${QM_FILES} PROPERTIES GENERATED TRUE)

# Add them to qrc; BASE is required
qt_add_resources(patronus translations
    PREFIX "/translations"
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES ${QM_FILES}
)

if(BUILD_APPIMAGE AND NOT ANDROID)
    # Override install layout: place the binary in the root and libraries in lib/
    install(TARGETS patronus
        RUNTIME DESTINATION .  # instead of bin/
    )
    
    # Copy custom libraries
    install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../corecrypto/build/libcorecrypto.so
        ${CMAKE_CURRENT_SOURCE_DIR}/../doubleratchet/build/libdoubleratchet.so
        DESTINATION lib
    )
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(patronus PRIVATE QT_NO_DEBUG_OUTPUT)
endif()