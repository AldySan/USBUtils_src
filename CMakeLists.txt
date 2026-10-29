cmake_minimum_required(VERSION 3.16)
project(USBUtils VERSION 0.0.1 LANGUAGES CXX)

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
)
target_include_directories(usbutils_core PUBLIC src/core)
target_compile_definitions(usbutils_core PUBLIC
    USBUTILS_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(usbutils_core PUBLIC user32)
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

    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)

    add_executable(usbutils-gui
        src/gui/main.cpp
        src/gui/mainwindow.cpp
        src/gui/mainwindow.h
    )
    target_link_libraries(usbutils-gui PRIVATE
        usbutils_core Qt${QT_VERSION_MAJOR}::Widgets)
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
