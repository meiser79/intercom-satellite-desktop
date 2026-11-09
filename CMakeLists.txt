cmake_minimum_required(VERSION 3.22)
project(intercom-satellite VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets Multimedia LinguistTools)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
# Makes this app's self-signed certificate; Qt has no API for that.
pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)

qt_standard_project_setup()

qt_add_executable(intercom-satellite
    src/main.cpp
    src/Wire.h src/Wire.cpp
    src/Peer.h
    src/Config.h src/Config.cpp
    src/HttpServer.h src/HttpServer.cpp
    src/Tls.h src/Tls.cpp
    src/Discovery.h src/Discovery.cpp
    src/Audio.h src/Audio.cpp
    src/VideoCodec.h src/VideoCodec.cpp
    src/Camera.h src/Camera.cpp
    src/VideoLink.h src/VideoLink.cpp
    src/Engine.h src/Engine.cpp
    src/Ringer.h src/Ringer.cpp
    src/MainWindow.h src/MainWindow.cpp
    src/CallView.h src/CallView.cpp
    src/SettingsDialog.h src/SettingsDialog.cpp
    src/HistoryDialog.h src/HistoryDialog.cpp
    src/Icons.h src/Icons.cpp
    src/Theme.h src/Theme.cpp
    src/Switch.h src/Switch.cpp
    resources/resources.qrc
)

qt_add_translations(intercom-satellite
    TS_FILES translations/intercom_de.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete)

target_compile_definitions(intercom-satellite PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(intercom-satellite PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::WebSockets Qt6::Multimedia
    PkgConfig::FFMPEG PkgConfig::CRYPTO)

include(GNUInstallDirs)
install(TARGETS intercom-satellite RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/intercom-satellite.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/icons/intercom-satellite.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
# Compressed here, as Debian wants it, also for a plain cmake --install; dh_compress leaves a .gz as
# it is.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/intercom-satellite.1.gz
    COMMAND gzip -9n -c ${CMAKE_CURRENT_SOURCE_DIR}/packaging/intercom-satellite.1
            > ${CMAKE_CURRENT_BINARY_DIR}/intercom-satellite.1.gz
    DEPENDS packaging/intercom-satellite.1)
add_custom_target(manpage ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/intercom-satellite.1.gz)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/intercom-satellite.1.gz
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

enable_testing()
qt_add_executable(wire-test tests/wire_test.cpp src/Wire.cpp src/Wire.h)
target_link_libraries(wire-test PRIVATE Qt6::Core)
add_test(NAME wire COMMAND wire-test)
