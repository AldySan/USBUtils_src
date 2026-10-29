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
    src/core/installer.cpp
    src/core/dnas.cpp
    src/core/gamedb.cpp
    src/core/recovery.cpp
    src/core/usbdevice.cpp
    src/core/videorip.cpp
    src/core/ps2load.cpp
    src/core/gameops.cpp
    src/core/update.cpp
)
target_include_directories(usbutils_core PUBLIC src/core)
target_compile_definitions(usbutils_core PUBLIC
    USBUTILS_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(usbutils_core PUBLIC user32 winhttp)
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
add_executable(usbutils src/cli/main.cpp)
target_link_libraries(usbutils PRIVATE usbutils_core)

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
    )
    target_link_libraries(usbutils-gui PRIVATE
        usbutils_core Qt${QT_VERSION_MAJOR}::Widgets)
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
