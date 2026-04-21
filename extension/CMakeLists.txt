cmake_minimum_required(VERSION 3.20)
project(reaper_uf8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ----- REAPER SDK + WDL (fetch if missing) ------------------------------------
# reaper_plugin.h on non-Windows includes "../WDL/swell/swell.h" relative to
# the sdk directory. We vendor both repos into vendor/ so the relative path
# resolves: vendor/reaper-sdk/sdk/reaper_plugin.h -> vendor/WDL/swell/swell.h
set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
set(REAPER_SDK_DIR "${VENDOR_DIR}/reaper-sdk")
set(WDL_DIR        "${VENDOR_DIR}/WDL")

include(FetchContent)
if(NOT EXISTS "${REAPER_SDK_DIR}/sdk/reaper_plugin.h")
    FetchContent_Declare(reaper_sdk
        GIT_REPOSITORY https://github.com/justinfrankel/reaper-sdk.git
        GIT_TAG main
        SOURCE_DIR "${REAPER_SDK_DIR}"
    )
    FetchContent_MakeAvailable(reaper_sdk)
endif()
if(NOT EXISTS "${WDL_DIR}/WDL/swell/swell.h")
    FetchContent_Declare(wdl
        GIT_REPOSITORY https://github.com/justinfrankel/WDL.git
        GIT_TAG main
        SOURCE_DIR "${WDL_DIR}"
    )
    FetchContent_MakeAvailable(wdl)
endif()

# reaper_plugin.h does `#include "../WDL/swell/swell.h"` — expects WDL to
# live as a sibling of the sdk/ directory. Make that true with a symlink.
if(NOT EXISTS "${REAPER_SDK_DIR}/WDL")
    file(CREATE_LINK "${WDL_DIR}/WDL" "${REAPER_SDK_DIR}/WDL" SYMBOLIC)
endif()

# ----- libusb -----------------------------------------------------------------
# On macOS: brew install libusb. On Linux: apt install libusb-1.0-0-dev.
# On Windows: vcpkg install libusb or FetchContent the prebuilt MSVC build.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
    pkg_check_modules(HIDAPI REQUIRED IMPORTED_TARGET hidapi)
endif()

# ----- the plugin -------------------------------------------------------------
add_library(reaper_uf8 MODULE
    src/main.cpp
    src/Protocol.cpp
    src/Palette.cpp
    src/PluginMap.cpp
    src/UF8Device.cpp
    src/ColorSync.cpp
    src/MidiBridge.cpp
    src/HidDevice.cpp
)

target_link_libraries(reaper_uf8 PRIVATE PkgConfig::HIDAPI)

if(APPLE)
    target_link_libraries(reaper_uf8 PRIVATE
        "-framework CoreMIDI"
        "-framework CoreFoundation"
        "-framework ApplicationServices")
endif()

target_include_directories(reaper_uf8 PRIVATE
    src
    "${REAPER_SDK_DIR}/sdk"
)

target_link_libraries(reaper_uf8 PRIVATE PkgConfig::LIBUSB)

# REAPER loads libraries named "reaper_*.dylib" on macOS
set_target_properties(reaper_uf8 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "reaper_uf8"
)

if(APPLE)
    set_target_properties(reaper_uf8 PROPERTIES SUFFIX ".dylib")
elseif(WIN32)
    set_target_properties(reaper_uf8 PROPERTIES SUFFIX ".dll")
endif()

# ----- tests (protocol-only, no libusb/REAPER) --------------------------------
add_executable(test_protocol tests/test_protocol.cpp src/Protocol.cpp src/Palette.cpp)
target_include_directories(test_protocol PRIVATE src)
enable_testing()
add_test(NAME protocol COMMAND test_protocol)

# ----- CLI smoke-test (libusb only, no REAPER) --------------------------------
add_executable(uf8_color_test tools/uf8_color_test.cpp src/Protocol.cpp)
target_include_directories(uf8_color_test PRIVATE src)
target_link_libraries(uf8_color_test PRIVATE PkgConfig::LIBUSB)

# ----- Palette probe (interactive) --------------------------------------------
add_executable(uf8_palette_probe tools/uf8_palette_probe.cpp src/Protocol.cpp)
target_include_directories(uf8_palette_probe PRIVATE src)
target_link_libraries(uf8_palette_probe PRIVATE PkgConfig::LIBUSB)

# ----- HID probe tool ---------------------------------------------------------
add_executable(uf8_hid_probe tools/uf8_hid_probe.cpp)
target_link_libraries(uf8_hid_probe PRIVATE PkgConfig::HIDAPI)
