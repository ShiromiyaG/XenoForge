# FFmpeg's XMA frame decoder (plan step A6; docs/runtime.md, "Sound"), the one
# Xenia decodes XMA with: has207's fork of FFmpeg (branch xmaframes), pinned to
# the commit xenia-canary's third_party/FFmpeg submodule is at. Only the part
# of libavutil and libavcodec that the xmaframes decoder needs is built, the way
# xenia-canary's third_party/CMakeLists.txt builds its FFmpeg subset
# (ffmpeg_common), over the configuration overlay in this directory (after
# xenia-canary's third_party/ffmpeg-xenia).
#
# FFmpeg is LGPL 2.1 or later (LICENSE-ffmpeg). It is built as a shared library
# of its own, ffmpeg-xma (.dll / .so), that the core links to dynamically, so a
# program shipped with it can have the library replaced by another build of
# these sources (docs/runtime.md, "Sound", says what distributing it takes).
#
# XF_FFMPEG_DIR names a checkout of the fork to build from instead of fetching
# it (xenia-canary's submodule is one; its sibling checkout is used by default
# when it is there at the pinned commit).

set(XF_FFMPEG_COMMIT d980192e175e6ff95bcd287af77e16fcb6597974)
set(XF_FFMPEG_DIR "" CACHE PATH "a checkout of has207/FFmpeg at ${XF_FFMPEG_COMMIT} (empty: fetched)")

set(_ffmpeg_dir "${XF_FFMPEG_DIR}")
if(NOT _ffmpeg_dir)
    set(_xenia_ffmpeg "${CMAKE_CURRENT_SOURCE_DIR}/../xenia-canary/third_party/FFmpeg")
    if(EXISTS "${_xenia_ffmpeg}/libavcodec/wmaprodec.c")
        find_package(Git QUIET)
        if(GIT_FOUND)
            execute_process(COMMAND ${GIT_EXECUTABLE} -C "${_xenia_ffmpeg}" rev-parse HEAD
                            OUTPUT_VARIABLE _xenia_ffmpeg_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(_xenia_ffmpeg_head STREQUAL XF_FFMPEG_COMMIT)
                set(_ffmpeg_dir "${_xenia_ffmpeg}")
            endif()
        endif()
    endif()
endif()
if(_ffmpeg_dir)
    if(NOT EXISTS "${_ffmpeg_dir}/libavcodec/wmaprodec.c")
        message(FATAL_ERROR "XF_FFMPEG_DIR: no FFmpeg checkout at ${_ffmpeg_dir}")
    endif()
    file(STRINGS "${_ffmpeg_dir}/libavcodec/wmaprodec.c" _xmaframes REGEX "ff_xmaframes_decoder")
    if(NOT _xmaframes)
        message(FATAL_ERROR "XF_FFMPEG_DIR: ${_ffmpeg_dir} is not has207's fork (no xmaframes decoder)")
    endif()
    set(XF_FFMPEG_SOURCE "${_ffmpeg_dir}")
else()
    # the commit's archive: a few tens of MB, where the repository is hundreds
    # (FFmpeg has no CMakeLists.txt, so this only downloads and unpacks it)
    include(FetchContent)
    FetchContent_Declare(xf_ffmpeg URL https://github.com/has207/FFmpeg/archive/${XF_FFMPEG_COMMIT}.tar.gz
                         DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(xf_ffmpeg)
    set(XF_FFMPEG_SOURCE "${xf_ffmpeg_SOURCE_DIR}")
endif()
message(STATUS "XMA decoding: FFmpeg from ${XF_FFMPEG_SOURCE}")

set(_ffmpeg_overlay "${CMAKE_CURRENT_LIST_DIR}")
set(_f "${XF_FFMPEG_SOURCE}")

set(_avutil_sources
    avstring.c audio_fifo.c bprint.c buffer.c channel_layout.c cpu.c crc.c dict.c error.c eval.c fifo.c
    file_open.c float_dsp.c frame.c hwcontext.c imgutils.c intmath.c log.c log2_tab.c mathematics.c mem.c opt.c
    parseutils.c pixdesc.c rational.c reverse.c samplefmt.c side_data.c refstruct.c slicethread.c time.c tx.c
    tx_float.c tx_double.c tx_int32.c utils.c channel_layout.c csp.c display.c random_seed.c sha.c avsscanf.c
    container_fifo.c video_enc_params.c film_grain_params.c timecode.c timecode_internal.c float_scalarproduct.c
    hdr_dynamic_metadata.c mastering_display_metadata.c downmix_info.c spherical.c stereo3d.c dovi_meta.c
    encryption_info.c iamf.c timestamp.c lfg.c md5.c fixed_dsp.c)
set(_avcodec_sources
    allcodecs.c avcodec.c bitstream.c bitstream_filters.c bsf.c bsf/null.c codec_desc.c codec_par.c decode.c
    encode.c get_buffer.c options.c packet.c parser.c parsers.c profiles.c pthread.c pthread_frame.c pthread_slice.c
    threadprogress.c utils.c vlc.c sinewin.c wma.c wma_common.c wma_freqs.c wmaprodec.c mathtables.c raw.c exif.c
    tiff_common.c)
list(REMOVE_DUPLICATES _avutil_sources)
list(TRANSFORM _avutil_sources PREPEND "${_f}/libavutil/")
list(TRANSFORM _avcodec_sources PREPEND "${_f}/libavcodec/")
# wmaprodec.c is compiled through the overlay's libavcodec/xf_wmaprodec.c,
# which includes it unchanged and adds xf_xmaframes_state_size
list(REMOVE_ITEM _avcodec_sources "${_f}/libavcodec/wmaprodec.c")
list(APPEND _avcodec_sources "${_ffmpeg_overlay}/libavcodec/xf_wmaprodec.c")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|amd64")
    # (tx_float_init.c only lists the assembly codelets, not built: no x86asm)
    foreach(s cpu.c fixed_dsp_init.c float_dsp_init.c imgutils_init.c)
        list(APPEND _avutil_sources "${_f}/libavutil/x86/${s}")
    endforeach()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    list(APPEND _avutil_sources "${_f}/libavutil/aarch64/cpu.c")
endif()

add_library(ffmpeg_xma SHARED ${_avutil_sources} ${_avcodec_sources})
set_target_properties(ffmpeg_xma PROPERTIES OUTPUT_NAME ffmpeg-xma LINKER_LANGUAGE C
                                            WINDOWS_EXPORT_ALL_SYMBOLS ON C_VISIBILITY_PRESET default
                                            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
                                            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_compile_definitions(ffmpeg_xma PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
target_include_directories(ffmpeg_xma PRIVATE ${_ffmpeg_overlay} ${_f} ${_f}/libavcodec)
if(WIN32)
    target_include_directories(ffmpeg_xma PRIVATE ${_f}/compat/atomics/win32)
    target_link_libraries(ffmpeg_xma PRIVATE bcrypt)
else()
    target_include_directories(ffmpeg_xma PRIVATE ${_f}/compat/atomics/gcc)
    target_link_libraries(ffmpeg_xma PRIVATE m pthread)
endif()
target_compile_options(ffmpeg_xma PRIVATE -w) # FFmpeg's sources, not the project's warnings
# the public headers, for the decoder in the core (avconfig.h is the overlay's)
target_include_directories(ffmpeg_xma SYSTEM INTERFACE ${_ffmpeg_overlay} ${_f})
