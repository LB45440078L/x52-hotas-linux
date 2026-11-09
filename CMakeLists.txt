cmake_minimum_required(VERSION 3.20)
project(x52pro-linux VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

# ---------------------------------------------------------------------------
# Core library: protocol, device, MFD, LEDs, clock, input, profiles, engine
# ---------------------------------------------------------------------------
add_library(x52core STATIC
    src/core/protocol.cpp
    src/core/device.cpp
    src/core/mfd.cpp
    src/core/leds.cpp
    src/core/clock.cpp
    src/core/input.cpp
    src/core/uinput.cpp
    src/core/profile.cpp
    src/core/engine.cpp
    src/core/client.cpp
    src/core/diagnostics.cpp
    src/core/settings.cpp
    src/core/telemetry.cpp
    src/core/rules.cpp
)
target_include_directories(x52core PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(x52core PUBLIC PkgConfig::LIBUSB)
target_compile_definitions(x52core PUBLIC X52_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Hardware diagnostic / test tool
# ---------------------------------------------------------------------------
add_executable(x52probe src/probe/main.cpp)
target_link_libraries(x52probe PRIVATE x52core)

# ---------------------------------------------------------------------------
# Optional components
# ---------------------------------------------------------------------------
option(BUILD_DAEMON "Build x52d" ON)
option(BUILD_CLI "Build x52ctl" ON)
option(BUILD_GUI "Build the GTK4 control panel" ON)
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_DAEMON AND EXISTS ${CMAKE_SOURCE_DIR}/src/daemon/main.cpp)
    add_executable(x52d src/daemon/main.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(x52d PRIVATE x52core Threads::Threads)
    target_compile_definitions(x52d PRIVATE X52_DEFAULT_PROFILE_DIR="${CMAKE_INSTALL_DATADIR}")
    install(TARGETS x52d RUNTIME DESTINATION bin)
endif()

if(BUILD_CLI AND EXISTS ${CMAKE_SOURCE_DIR}/src/cli/main.cpp)
    add_executable(x52ctl src/cli/main.cpp)
    target_link_libraries(x52ctl PRIVATE x52core)
    install(TARGETS x52ctl RUNTIME DESTINATION bin)
endif()

if(BUILD_GUI AND EXISTS ${CMAKE_SOURCE_DIR}/src/gui/main.cpp)
    pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
    pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1)
    add_executable(x52panel src/gui/main.cpp)
    target_link_libraries(x52panel PRIVATE x52core PkgConfig::GTK4 PkgConfig::ADW)
    install(TARGETS x52panel RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_executable(x52_tests
        tests/test_main.cpp
        tests/test_protocol.cpp
        tests/test_mfd.cpp
        tests/test_profile.cpp
        tests/test_engine.cpp
        tests/test_clock.cpp
        tests/test_diagnostics.cpp
        tests/test_telemetry.cpp
    )
    target_link_libraries(x52_tests PRIVATE x52core)
    target_include_directories(x52_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    add_test(NAME unit COMMAND x52_tests)

    # Hardware integration test: not registered with ctest, needs the device.
    add_executable(x52_it tests/integration.cpp)
    target_link_libraries(x52_it PRIVATE x52core)
endif()

install(TARGETS x52probe RUNTIME DESTINATION bin)

include(GNUInstallDirs)
install(DIRECTORY data/udev/ DESTINATION ${CMAKE_INSTALL_DATADIR}/x52pro/udev)
install(DIRECTORY data/systemd/ DESTINATION ${CMAKE_INSTALL_DATADIR}/x52pro/systemd)
install(DIRECTORY data/applications/ DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(DIRECTORY data/icons/ DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(DIRECTORY data/profiles/ DESTINATION ${CMAKE_INSTALL_DATADIR}/x52pro/profiles)
