# A game's generated code as a static library (xenoforge_add_port): configured
# by the port's build over what `xfrecomp emit` wrote, so the glob sees every
# file. Parameters: XF_GAME_NAME, XF_GAME_CODE (the emission's directory),
# XF_GAME_OUT (where the archive goes), XF_INCLUDE_DIR (XenoForge's headers),
# XF_GAME_OPT.
cmake_minimum_required(VERSION 3.25)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
project(xenoforge_game_${XF_GAME_NAME} LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

file(GLOB sources ${XF_GAME_CODE}/modules.cpp ${XF_GAME_CODE}/*/functions_*.cpp ${XF_GAME_CODE}/*/table.cpp)
if(NOT sources)
    message(FATAL_ERROR "no generated code in ${XF_GAME_CODE}")
endif()
add_library(game STATIC ${sources})
target_include_directories(game PRIVATE ${XF_INCLUDE_DIR})
# optimized, without debug information (it is large) or warnings; never -ffast-math
target_compile_options(game PRIVATE ${XF_GAME_OPT} -g0 -w -mfma -mavx2)
set_target_properties(game PROPERTIES OUTPUT_NAME game_${XF_GAME_NAME} ARCHIVE_OUTPUT_DIRECTORY ${XF_GAME_OUT})
