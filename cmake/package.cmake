# The player as it is shipped (docs/runtime.md, "Shipping the player"): one
# folder with the player executables, ffmpeg-xma (when built with XF_XMA) and
# every notice the binaries owe.
#
#   cmake --install build --prefix <folder>     the folder
#   cmake --build build --target player_package build/xenoforge-player/ and build/xenoforge-player.zip
#
# What goes in:
#   player.exe                   XenoForge (MIT); player_<game>.exe only with XF_PACKAGE_GAME_PLAYERS
#   launcher.exe                 XenoForge (MIT): the games, their settings, each started in its player
#   ffmpeg-xma.dll               FFmpeg (LGPL 2.1 or later), loaded at run time, replaceable
#   LICENSE                      XenoForge's (MIT)
#   LICENSE-ffmpeg               which FFmpeg, and the LGPL 2.1's text
#   COPYING.LGPLv2.1             the LGPL 2.1 as the FFmpeg checkout built carries it
#   FFMPEG-SOURCE.txt            where ffmpeg-xma's source is: the fork's commit and the overlay
#   ffmpeg-source/               the overlay itself (third_party/ffmpeg: configuration and build script)
#   LICENSE-xenia, LICENSE-rexglue
#   THIRD-PARTY-NOTICES.txt      SDL3 (with its HIDAPI), Dear ImGui, toml++, stb, linked into the player
#
# Included by CMakeLists.txt once the player's targets (XF_PLAYERS) and its
# fetched dependencies exist.

set(XF_PACKAGE_DIR "${CMAKE_BINARY_DIR}/xenoforge-player")

# player_<game> carries that game's recompiled code: a game's, never
# XenoForge's to hand out (docs/plan.md, principle 5: ports are built on the
# player's machine from their own disc). So the folder has the player alone,
# which runs any game interpreted, unless XF_PACKAGE_GAME_PLAYERS asks for the
# game players too, for a folder that stays on the machine that built them.
option(XF_PACKAGE_GAME_PLAYERS "install player_<game> too (game code: never to distribute)" OFF)
set(_installed player launcher)
if(XF_PACKAGE_GAME_PLAYERS)
    set(_installed ${XF_PLAYERS} launcher)
endif()
install(TARGETS ${_installed} RUNTIME DESTINATION .)
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/LICENSE-xenia ${CMAKE_SOURCE_DIR}/LICENSE-rexglue
        DESTINATION .)

# The libraries linked into the player, each with its license as fetched
set(_notices "${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt")
set(_rule "-------------------------------------------------------------------------------")
file(READ "${sdl3_SOURCE_DIR}/LICENSE.txt" _sdl)
file(READ "${sdl3_SOURCE_DIR}/src/hidapi/LICENSE-bsd.txt" _hidapi)
file(READ "${imgui_SOURCE_DIR}/LICENSE.txt" _imgui)
file(READ "${tomlplusplus_SOURCE_DIR}/LICENSE" _toml)
file(READ "${STB_DIR}/stb_image_write.h" _stb)
string(FIND "${_stb}" "This software is available under 2 licenses" _stb_at)
if(_stb_at LESS 0)
    message(FATAL_ERROR "package: no license in ${STB_DIR}/stb_image_write.h")
endif()
string(SUBSTRING "${_stb}" ${_stb_at} -1 _stb)
string(REGEX REPLACE "-*\n\\*/[ \t\r\n]*$" "" _stb "${_stb}")
file(WRITE "${_notices}"
"The XenoForge player links these libraries into its executables. Each one's
license follows.

${_rule}
SDL 3 (release-3.4.16), https://libsdl.org - zlib license
${_rule}
${_sdl}
${_rule}
HIDAPI, part of SDL 3 (src/hidapi), https://github.com/libusb/hidapi - used
under its BSD-style license, one of the three it offers
${_rule}
${_hidapi}
${_rule}
Dear ImGui (v1.91.9), https://github.com/ocornut/imgui - MIT license
${_rule}
${_imgui}
${_rule}
toml++ (v3.4.0), https://github.com/marzer/tomlplusplus - MIT license
${_rule}
${_toml}
${_rule}
stb_image_write, https://github.com/nothings/stb - MIT license or public domain
${_rule}
${_stb}
")
install(FILES "${_notices}" DESTINATION .)

if(XF_XMA)
    # the library, its license, and where its source is (written at install
    # time, with the commit XenoForge is at then)
    install(FILES $<TARGET_FILE:ffmpeg_xma> DESTINATION .)
    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE-ffmpeg ${XF_FFMPEG_SOURCE}/COPYING.LGPLv2.1 DESTINATION .)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/ DESTINATION ffmpeg-source)
    # the commit actually built: the checkout's (XF_FFMPEG_DIR) when it is
    # one, else the archive's, the pinned one
    set(_ffmpeg_built ${XF_FFMPEG_COMMIT})
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS "${XF_FFMPEG_SOURCE}/.git")
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD WORKING_DIRECTORY ${XF_FFMPEG_SOURCE}
                        OUTPUT_VARIABLE _ffmpeg_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_ffmpeg_head)
            set(_ffmpeg_built ${_ffmpeg_head})
        endif()
    endif()
    if(NOT _ffmpeg_built STREQUAL XF_FFMPEG_COMMIT)
        message(WARNING "package: FFmpeg is built from ${_ffmpeg_built}, not the pinned ${XF_FFMPEG_COMMIT}; "
                        "FFMPEG-SOURCE.txt names the one built")
    endif()
    install(CODE "set(XF_SOURCE_DIR \"${CMAKE_SOURCE_DIR}\")
set(XF_FFMPEG_COMMIT \"${_ffmpeg_built}\")
set(XF_FFMPEG_FILE \"$<TARGET_FILE_NAME:ffmpeg_xma>\")
set(XF_COMPILER \"${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}\")")
    install(SCRIPT ${CMAKE_SOURCE_DIR}/cmake/ffmpeg_source.cmake)
endif()

# the folder in the build directory, and a zip of it
add_custom_target(player_package
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${XF_PACKAGE_DIR}
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --prefix ${XF_PACKAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E tar cf ${CMAKE_BINARY_DIR}/xenoforge-player.zip --format=zip -- xenoforge-player
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS ${_installed}
    COMMENT "the player's folder: ${XF_PACKAGE_DIR}"
    VERBATIM)
if(XF_XMA)
    add_dependencies(player_package ffmpeg_xma)
endif()
