# A port in its own repository, built on an installed XenoForge (docs/sdk.md).
#
#   cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<XenoForge's prefix> -DMINIMAL_ISO=<disc image>
#   cmake --build build
#   build/player_minimal <disc image>        build/boot_minimal <disc image> --seconds 20
#
# The first build recompiles the disc (xfrecomp emit, then clang over the
# generated code: minutes); after that only what the ISO, the project file or
# XenoForge changed.
cmake_minimum_required(VERSION 3.25)
# optimized unless asked otherwise: XenoForge's libraries are built for the release runtime
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
project(minimal_port LANGUAGES CXX)

find_package(XenoForge REQUIRED)

set(MINIMAL_ISO "" CACHE FILEPATH "the game's disc image")
# an emission made earlier (xfrecomp emit <iso> <dir>), compiled as it is, instead of the ISO's (the SDK's own test
# builds the example this way, on a table with no module, to check the package without a game)
set(MINIMAL_GENERATED_DIR "" CACHE PATH "a directory xfrecomp emit wrote")
# the project file (docs/recompiler.md, "The project file"), passed to xfrecomp emit when set
set(MINIMAL_PROJECT "" CACHE FILEPATH "the port's project file")

if(MINIMAL_GENERATED_DIR)
    xenoforge_add_port(minimal GENERATED_DIR ${MINIMAL_GENERATED_DIR} SOURCES main.cpp)
elseif(MINIMAL_ISO)
    if(MINIMAL_PROJECT)
        xenoforge_add_port(minimal ISO ${MINIMAL_ISO} PROJECT ${MINIMAL_PROJECT} SOURCES main.cpp)
    else()
        xenoforge_add_port(minimal ISO ${MINIMAL_ISO} SOURCES main.cpp)
    endif()
else()
    message(FATAL_ERROR "set MINIMAL_ISO to the game's disc image")
endif()
