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
#   toolchain/bin/               clang and lld (LLVM, Apache 2.0 with LLVM exceptions; toolchain/LICENSE-llvm.txt),
#                                which Prepare builds the game's library with (XF_PACKAGE_TOOLCHAIN, on)
#   ui/                          the launcher's windows (RML, RCSS), its fonts, Inter (SIL OFL) and Roboto Condensed (Apache 2.0), and icons, Lucide (ISC)
#   THIRD-PARTY-NOTICES.txt      SDL3 (with its HIDAPI), Dear ImGui, toml++, RmlUi, FreeType, Inter, Roboto Condensed, Lucide, stb, the cursors (public domain),
#                                FidelityFX, FXAA, volk, Vulkan-Headers
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

# The toolchain the launcher's Prepare builds a game's library with
# (docs/runtime.md, "The toolchain shipped"): clang and lld from the LLVM that
# builds XenoForge, into toolchain/bin, where find_toolchain looks first after
# --toolchain and XF_TOOLCHAIN. Without it a machine with no LLVM installed
# can only run games interpreted. lld is copied once (run as `lld -flavor link`
# when there is no lld-link).
option(XF_PACKAGE_TOOLCHAIN "ship clang and lld in toolchain/bin" ON)
if(XF_PACKAGE_TOOLCHAIN)
    get_filename_component(_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_clang "${_llvm_bin}/clang${CMAKE_EXECUTABLE_SUFFIX}")
    set(_lld "${_llvm_bin}/lld${CMAKE_EXECUTABLE_SUFFIX}")
    if(NOT EXISTS "${_clang}" OR NOT EXISTS "${_lld}")
        message(FATAL_ERROR "XF_PACKAGE_TOOLCHAIN: no clang and lld beside ${CMAKE_CXX_COMPILER} "
                            "(build with LLVM's clang, or -DXF_PACKAGE_TOOLCHAIN=OFF for a package without them)")
    endif()
    install(PROGRAMS "${_clang}" "${_lld}" DESTINATION toolchain/bin)
    install(FILES ${CMAKE_SOURCE_DIR}/third_party/llvm/LICENSE.TXT DESTINATION toolchain RENAME LICENSE-llvm.txt)
endif()

# The libraries linked into the player, each with its license as fetched
set(_notices "${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt")
set(_rule "-------------------------------------------------------------------------------")
file(READ "${sdl3_SOURCE_DIR}/LICENSE.txt" _sdl)
file(READ "${sdl3_SOURCE_DIR}/src/hidapi/LICENSE-bsd.txt" _hidapi)
file(READ "${imgui_SOURCE_DIR}/LICENSE.txt" _imgui)
file(READ "${tomlplusplus_SOURCE_DIR}/LICENSE" _toml)
file(READ "${rmlui_SOURCE_DIR}/LICENSE.txt" _rmlui)
file(READ "${freetype_SOURCE_DIR}/docs/FTL.TXT" _freetype)
file(READ "${CMAKE_SOURCE_DIR}/frontend/ui/fonts/LICENSE-Inter.txt" _inter)
file(READ "${CMAKE_SOURCE_DIR}/frontend/ui/fonts/LICENSE-Lucide.txt" _lucide)
file(READ "${CMAKE_SOURCE_DIR}/third_party/FidelityFX-FSR/LICENSE.txt" _fsr)
file(READ "${CMAKE_SOURCE_DIR}/third_party/FidelityFX-CAS/LICENSE.txt" _cas)
file(READ "${CMAKE_SOURCE_DIR}/third_party/fxaa/LICENSE" _fxaa)
file(READ "${volk_SOURCE_DIR}/LICENSE.md" _volk)
file(READ "${vulkanheaders_SOURCE_DIR}/LICENSES/Apache-2.0.txt" _vulkan_headers)
file(READ "${CMAKE_SOURCE_DIR}/frontend/ui/fonts/LICENSE-Roboto.txt" _roboto)
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
RmlUi (6.3), https://github.com/mikke89/RmlUi - MIT license (the launcher's interface)
${_rule}
${_rmlui}
${_rule}
FreeType (VER-2-14-3), https://freetype.org - used under the FreeType License
(FTL). Portions of this software are copyright The FreeType Project
(www.freetype.org). All rights reserved.
${_rule}
${_freetype}
${_rule}
Inter (4.1), https://rsms.me/inter - the launcher's typeface, SIL Open Font
License 1.1, shipped as ui/fonts/Inter-*.ttf
${_rule}
${_inter}
${_rule}
Roboto Condensed (2.138), https://github.com/googlefonts/roboto-2 - the launcher's
condensed typeface for the games and the toolbar, Apache License 2.0, shipped as
ui/fonts/RobotoCondensed-*.ttf
${_rule}
${_roboto}
${_rule}
Lucide (lucide-static 0.460.0), https://lucide.dev - the launcher's icons, ISC
license (portions from Feather, MIT), shipped as ui/fonts/Lucide.ttf
${_rule}
${_lucide}
${_rule}
Xbox 360 web cursors by MiniYak_YT, https://www.rw-designer.com/cursor-set/xbox-360-custom-web - the launcher's
pointers, released to the public domain by the author, shipped as ui/cursors/ with the author's note (README.txt)
${_rule}
stb_image and stb_image_write, https://github.com/nothings/stb - MIT license or public domain
${_rule}
${_stb}
${_rule}
AMD FidelityFX FSR 1 (ffx_a.h, ffx_fsr1.h), https://github.com/GPUOpen-Effects/FidelityFX-FSR - MIT
license (the player's FSR output scaling)
${_rule}
${_fsr}
${_rule}
AMD FidelityFX CAS (ffx_a.h, ffx_cas.h), https://github.com/GPUOpen-Effects/FidelityFX-CAS - MIT
license (the player's CAS output scaling)
${_rule}
${_cas}
${_rule}
NVIDIA FXAA 3.11 (FXAA3_11.h) - BSD 3-clause license (the player's FXAA)
${_rule}
${_fxaa}
${_rule}
volk (vulkan-sdk-${XF_VULKAN_SDK_TAG}.0), https://github.com/zeux/volk - MIT license (Vulkan's
functions loaded from the system's loader)
${_rule}
${_volk}
${_rule}
Vulkan-Headers (v${XF_VULKAN_SDK_TAG}), https://github.com/KhronosGroup/Vulkan-Headers - Apache
License 2.0 (the C headers compiled in; unchanged)
${_rule}
${_vulkan_headers}
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
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --component player --prefix ${XF_PACKAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E tar cf ${CMAKE_BINARY_DIR}/xenoforge-player.zip --format=zip -- xenoforge-player
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS ${_installed}
    COMMENT "the player's folder: ${XF_PACKAGE_DIR}"
    VERBATIM)
if(XF_XMA)
    add_dependencies(player_package ffmpeg_xma)
endif()
