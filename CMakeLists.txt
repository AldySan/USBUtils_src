cmake_minimum_required(VERSION 3.16)
project(USBUtils VERSION 0.0.4 LANGUAGES CXX)

option(USBUTILS_BUILD_GUI "Build the Qt Widgets GUI" ON)
option(USBUTILS_BUILD_TESTS "Build unit tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# ---------------------------------------------------------------------------
# Core library (no external dependencies)
# ---------------------------------------------------------------------------
add_library(usbutils_core STATIC
    src/core/ulcfg.cpp
    src/core/ulid.cpp
    src/core/gamefile.cpp
    src/core/gamestream.cpp
    src/core/iso9660.cpp
    src/core/isoedit.cpp
    src/core/installer.cpp
    src/core/dnas.cpp
    src/core/gamedb.cpp
    src/core/recovery.cpp
    src/core/usbdevice.cpp
    src/core/videorip.cpp
    src/core/ps2load.cpp
    src/core/gameops.cpp
    src/core/update.cpp
    src/core/supportfiles.cpp
)
target_include_directories(usbutils_core PUBLIC src/core)
target_compile_definitions(usbutils_core PUBLIC
    USBUTILS_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(usbutils_core PUBLIC user32 winhttp)
endif()

# ---------------------------------------------------------------------------
# Shared resources: application icon
#   res/usbutils.ico        Windows exe icon (embedded via RC/windres)
#   res/icons/*.png|.svg    hicolor icons for Linux packaging + .desktop
#   res/usbutils.qrc        compiled-in Qt resource (window icon anywhere)
# ---------------------------------------------------------------------------
set(PROJECT_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(PROJECT_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(PROJECT_VERSION_PATCH ${PROJECT_VERSION_PATCH})

function(usbutils_add_exe_icon TARGET DESC NAME)
    if(WIN32)
        # Embed the .ico + VERSIONINFO via windres. MXE/MinGW toolchains
        # enable the RC language automatically; guard for toolchains that
        # do not, so the build never breaks just because of a missing icon.
        include(CheckLanguage)
        check_language(RC)
        if(CMAKE_RC_COMPILER)
            enable_language(RC)
            # bake version + icon path into a configured .rc (plain
            # literals — windres needs no preprocessor string tricks)
            set(USBUTILS_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/usbutils.ico")
            set(USBUTILS_VER_MAJOR "${PROJECT_VERSION_MAJOR}")
            set(USBUTILS_VER_MINOR "${PROJECT_VERSION_MINOR}")
            set(USBUTILS_VER_PATCH "${PROJECT_VERSION_PATCH}")
            set(USBUTILS_VER_STRING "${PROJECT_VERSION}")
            set(USBUTILS_RC_DESC "${DESC}")
            set(USBUTILS_RC_NAME "${NAME}")
            set(USBUTILS_RC_OUT "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_icon.rc")
            configure_file(res/usbutils.rc.in "${USBUTILS_RC_OUT}" @ONLY)
            target_sources(${TARGET} PRIVATE "${USBUTILS_RC_OUT}")
        else()
            message(WARNING "No RC compiler found - '${TARGET}' gets no embedded icon")
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
add_executable(usbutils src/cli/main.cpp)
target_link_libraries(usbutils PRIVATE usbutils_core)
usbutils_add_exe_icon(usbutils "USBUtils command line tool" "usbutils.exe")

# ---------------------------------------------------------------------------
# GUI (Qt Widgets) — disable with -DUSBUTILS_BUILD_GUI=OFF when Qt is absent
# ---------------------------------------------------------------------------
if(USBUTILS_BUILD_GUI)
    find_package(QT NAMES Qt6 Qt5 COMPONENTS Widgets REQUIRED)
    find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Widgets REQUIRED)

    # --- cross-build support --------------------------------------------
    # When cross-compiling (e.g. building the Windows GUI from Linux),
    # the target kit's tool executables (moc.exe) cannot run on the host.
    # Point USBUTILS_HOST_QT_PREFIX at a HOST Qt install of the same minor
    # version to reuse its tools (distro layouts: <prefix>/lib/qt6/libexec).
    set(USBUTILS_HOST_MOC "")
    if(CMAKE_CROSSCOMPILING AND USBUTILS_HOST_QT_PREFIX)
        set(_host_libexec "${USBUTILS_HOST_QT_PREFIX}/lib/qt6/libexec")
        if(EXISTS "${_host_libexec}/moc")
            set(USBUTILS_HOST_MOC "${_host_libexec}/moc")
            if(EXISTS "${_host_libexec}/uic")
                set(USBUTILS_HOST_UIC "${_host_libexec}/uic")
            endif()
            if(EXISTS "${_host_libexec}/rcc")
                set(USBUTILS_HOST_RCC "${_host_libexec}/rcc")
            endif()
            message(STATUS "Cross build: using host moc from ${_host_libexec}")
        else()
            message(WARNING "USBUTILS_HOST_QT_PREFIX set but no moc found in ${_host_libexec}")
        endif()
    endif()

    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)

    add_executable(usbutils-gui WIN32
        src/gui/main.cpp
        src/gui/mainwindow.cpp
        src/gui/mainwindow.h
        src/gui/videoripdialog.cpp
        src/gui/videoripdialog.h
        src/gui/dnasdialog.cpp
        src/gui/dnasdialog.h
        src/gui/ps2loaddialog.cpp
        src/gui/ps2loaddialog.h
        src/gui/finddialog.cpp
        src/gui/finddialog.h
        src/gui/gamefilesdialog.cpp
        src/gui/gamefilesdialog.h
        src/gui/isoexplorerdialog.cpp
        src/gui/isoexplorerdialog.h
        src/gui/configdialog.cpp
        src/gui/configdialog.h
        res/usbutils.qrc
    )
    target_link_libraries(usbutils-gui PRIVATE
        usbutils_core Qt${QT_VERSION_MAJOR}::Widgets)
    usbutils_add_exe_icon(usbutils-gui "USBUtils - PS2 USBExtreme manager (GUI)" "usbutils-gui.exe")
    if(USBUTILS_HOST_MOC)
        # run the HOST Qt tools (the target kit's .exe tools cannot
        # execute on the build machine)
        set_target_properties(usbutils-gui PROPERTIES
            AUTOMOC_EXECUTABLE "${USBUTILS_HOST_MOC}")
        if(USBUTILS_HOST_UIC)
            set_target_properties(usbutils-gui PROPERTIES
                AUTOUIC_EXECUTABLE "${USBUTILS_HOST_UIC}")
        endif()
        if(USBUTILS_HOST_RCC)
            set_target_properties(usbutils-gui PROPERTIES
                AUTORCC_EXECUTABLE "${USBUTILS_HOST_RCC}")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(USBUTILS_BUILD_TESTS)
    enable_testing()
    add_executable(usbutils_tests tests/test_core.cpp)
    target_link_libraries(usbutils_tests PRIVATE usbutils_core)
    add_test(NAME core COMMAND usbutils_tests)
endif()

install(TARGETS usbutils RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Linux desktop integration (icon + .desktop; used by release packaging)
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE AND USBUTILS_BUILD_GUI)
    foreach(_s 16 24 32 48 64 128 256)
        install(FILES res/icons/usbutils_${_s}.png
                DESTINATION share/icons/hicolor/${_s}x${_s}/apps
                RENAME usbutils.png)
    endforeach()
    install(FILES res/icons/usbutils.svg
            DESTINATION share/icons/hicolor/scalable/apps
            RENAME usbutils.svg)
    install(FILES res/linux/usbutils.desktop
            DESTINATION share/applications)
    install(PROGRAMS res/linux/usbutils-gui.sh
            DESTINATION bin
            RENAME usbutils-gui)
endif()
