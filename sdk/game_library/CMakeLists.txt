# A game's generated code as a static library (xenoforge_add_port): configured
# by the port's build over what `xfrecomp emit` wrote, so the glob sees every
# file. Parameters: XF_GAME_NAME, XF_GAME_CODE (the emission's directory),
# XF_GAME_OUT (where the archive goes), XF_INCLUDE_DIR (XenoForge's headers),
# XF_GAME_OPT, XF_GAME_CPU_FLAGS (the CPU level's flags: xenoforge_port.cmake),
# each a list or a space-separated string (build_game.cmake passes strings).
cmake_minimum_required(VERSION 3.25)
# Always an optimized configuration with the release C runtime, as XenoForge's
# libraries are built: the cache's own default (Debug, with CMake 4 and an
# MSVC-ABI clang) once took over when this project was configured again, and
# the generated code came out on the debug runtime, which the port's link
# refuses (_ITERATOR_DEBUG_LEVEL). Forced into the cache, whatever it held.
set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL CACHE STRING "" FORCE)
project(xenoforge_game_${XF_GAME_NAME} LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

file(GLOB sources ${XF_GAME_CODE}/modules.cpp ${XF_GAME_CODE}/*/functions_*.cpp ${XF_GAME_CODE}/*/table.cpp)
if(NOT sources)
    message(FATAL_ERROR "no generated code in ${XF_GAME_CODE}")
endif()
add_library(game STATIC ${sources})
target_include_directories(game PRIVATE ${XF_INCLUDE_DIR})
# optimized, without debug information (it is large) or warnings; never -ffast-math
foreach(v XF_GAME_OPT XF_GAME_CPU_FLAGS)
    if(NOT "${${v}}" MATCHES ";")
        separate_arguments(${v} UNIX_COMMAND "${${v}}")
    endif()
endforeach()
target_compile_options(game PRIVATE ${XF_GAME_OPT} -g0 -w ${XF_GAME_CPU_FLAGS})
set_target_properties(game PROPERTIES OUTPUT_NAME game_${XF_GAME_NAME} ARCHIVE_OUTPUT_DIRECTORY ${XF_GAME_OUT})
