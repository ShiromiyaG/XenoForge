# Run by `cmake --install` (package.cmake): writes FFMPEG-SOURCE.txt beside
# ffmpeg-xma, saying where the library's complete source is, as the LGPL asks
# of whoever ships it: has207's fork at the commit built, and XenoForge's
# overlay (shipped beside it in ffmpeg-source/, and in XenoForge's source at
# the commit named).

set(_commit "")
set(_dirty "")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD WORKING_DIRECTORY ${XF_SOURCE_DIR}
                    OUTPUT_VARIABLE _commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain -- third_party/ffmpeg
                    WORKING_DIRECTORY ${XF_SOURCE_DIR} OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
endif()
if(_commit STREQUAL "")
    set(_commit "(not known: built outside a git checkout)")
endif()
if(NOT _dirty STREQUAL "")
    set(_commit "${_commit}, with changes to third_party/ffmpeg not committed (the copy in ffmpeg-source/ is what was built)")
endif()

file(WRITE "${CMAKE_INSTALL_PREFIX}/FFMPEG-SOURCE.txt"
"${XF_FFMPEG_FILE} is FFmpeg (https://ffmpeg.org), under the GNU Lesser General
Public License version 2.1 or later (LICENSE-ffmpeg, COPYING.LGPLv2.1). Only
FFmpeg's LGPL parts are built into it: a part of libavutil and libavcodec, with
the xmaframes decoder that decodes the Xbox 360's XMA sound.

Its complete source is:

1. has207's fork of FFmpeg (branch xmaframes, the one Xenia uses), at commit
   ${XF_FFMPEG_COMMIT}
     https://github.com/has207/FFmpeg/tree/${XF_FFMPEG_COMMIT}
     https://github.com/has207/FFmpeg/archive/${XF_FFMPEG_COMMIT}.tar.gz

2. XenoForge's overlay over it, in ffmpeg-source/ beside this file and in
   XenoForge's source as third_party/ffmpeg (commit ${_commit}):
   the configuration headers (config.h, config_components.h,
   libavutil/avconfig.h, libavutil/ffversion.h, the codec, parser and
   bitstream filter lists), libavcodec/xf_wmaprodec.c (compiled in
   wmaprodec.c's place: it includes wmaprodec.c unchanged and adds one
   function, xf_xmaframes_state_size) and ffmpeg.cmake, the build script,
   which names every FFmpeg source file compiled into the library.

Built with ${XF_COMPILER}, by CMake, as XenoForge's CMakeLists.txt builds it
(XF_XMA on; XF_FFMPEG_DIR may name a checkout of the fork at that commit).

The XenoForge player is not linked to the library: it loads ${XF_FFMPEG_FILE}
from its own folder when it starts (or wherever the system finds libraries
when it is not there), looking up the FFmpeg functions it calls by name.
You may replace ${XF_FFMPEG_FILE} with your own build of these sources, changed
or not; without it the player runs and the games' XMA sound is silent.
")
