cmake_minimum_required(VERSION 3.20)
project(reaper_uf8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ----- REAPER SDK (fetch if missing) ------------------------------------------
set(REAPER_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/reaper-sdk")
if(NOT EXISTS "${REAPER_SDK_DIR}/sdk/reaper_plugin.h")
    include(FetchContent)
    FetchContent_Declare(reaper_sdk
        GIT_REPOSITORY https://github.com/justinfrankel/reaper-sdk.git
        GIT_TAG main
        SOURCE_DIR "${REAPER_SDK_DIR}"
    )
    FetchContent_MakeAvailable(reaper_sdk)
endif()

# ----- libusb -----------------------------------------------------------------
# On macOS: brew install libusb. On Linux: apt install libusb-1.0-0-dev.
# On Windows: vcpkg install libusb or FetchContent the prebuilt MSVC build.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
endif()

# ----- the plugin -------------------------------------------------------------
add_library(reaper_uf8 MODULE
    src/main.cpp
    src/Protocol.cpp
    src/Palette.cpp
    src/UF8Device.cpp
    src/ColorSync.cpp
)

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
