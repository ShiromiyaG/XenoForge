# xenoforge_add_port: a game's port built on XenoForge (docs/sdk.md). One file
# for both the XenoForge build tree (CMakeLists.txt, for XF_GAMES) and an
# installed XenoForge (XenoForgeConfig.cmake includes it), so a port and the
# repository's own games take the same path. It expects the targets
# XenoForge::core, ::kernel, ::vk and ::boot, ::player when the player is built,
# XenoForge::xfrecomp (or, cross-compiling, XENOFORGE_HOST_XFRECOMP: the
# build machine's), and XENOFORGE_INCLUDE_DIR and XENOFORGE_RUNTIME_FILES
# (what is copied next to each executable: ffmpeg-xma).
#
#   xenoforge_add_port(<name>
#       [ISO <disc image>] [XEX <path on the disc>] [PROJECT <project.toml>]
#       [SOURCES <port sources>...] [HOOK_SOURCES <hook sources>... [HOT_RELOAD]]
#       [DEFINITIONS ...] [INCLUDES ...] [LIBRARIES ...]
#       [OPT <-O2>] [NO_PLAYER] [NO_BOOT] [SHADER_STORAGE <title's .xfshaders>]
#       [GENERATED_DIR <dir> | PREBUILT <game library file>])
#
# Makes
#   game_<name>     the generated code as a static library: `xfrecomp emit` at
#                   build time into the build tree, then clang on what it wrote,
#                   both only when the ISO, the project file, xfrecomp or the
#                   generated code's headers changed (xfrecomp writes only the
#                   files whose text changed, so a rebuild recompiles only those);
#                   with GENERATED_DIR, the code already emitted there compiled in
#                   this build (CMake globs it); with PREBUILT, that library
#   player_<name>   SOURCES + the player + game_<name> (the window, input, sound); on
#                   Android a shared library, libplayer_<name>.so, whose SDL_main is the
#                   port's main (SDL's activity loads it from the APK)
#   boot_<name>     the same linked to boot (headless: tests, traces, --render)
# SOURCES holds the port's main (xenoforge/run.h's xf::run); it is compiled for
# each executable, boot's with XF_PORT_BOOT defined. The runtime files are copied
# next to both. HOOK_SOURCES are the port's hooks (xenoforge/port.h): compiled
# into both executables as SOURCES are, or with HOT_RELOAD into a module of
# their own, <name>_hooks (a DLL, a .so on POSIX) beside the executables, which
# load it as the game opens and again when it changes (xenoforge/port_module.h;
# docs/sdk.md, "Reloading the hooks"). The module links nothing of XenoForge:
# its sources see port.h alone, built with XF_PORT_MODULE.

set(_XF_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}")

# The CPU level the statically linked generated code is compiled for
# (xenoforge/cpu_level.h; a game library built by the recompile step takes
# the player's CPU's instead): v3 (x86-64-v3: AVX2, FMA, BMI2...) or v2
# (x86-64-v2: SSE4.2, POPCNT); ARM64's baseline on ARM64. The results are the
# same at every level. XF_GAME_CPU_FLAGS: the compiler's flags for it.
set(XF_GAME_CPU_LEVEL "v3" CACHE STRING "CPU level of the statically linked generated code: v3 or v2 (x86-64)")
set_property(CACHE XF_GAME_CPU_LEVEL PROPERTY STRINGS v3 v2)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(XF_GAME_CPU_FLAGS)
    set(_XF_GAME_CPU_LEVEL_NAME arm64)
elseif(XF_GAME_CPU_LEVEL STREQUAL "v3")
    # 128-bit vectors preferred (game_build_flags.h: 256-bit ones cost 10%)
    set(XF_GAME_CPU_FLAGS -march=x86-64-v3 -mprefer-vector-width=128)
elseif(XF_GAME_CPU_LEVEL STREQUAL "v2")
    set(XF_GAME_CPU_FLAGS -march=x86-64-v2)
else()
    message(FATAL_ERROR "XF_GAME_CPU_LEVEL=${XF_GAME_CPU_LEVEL}: v3 or v2")
endif()
# How many compilers build a port's generated code at once (its ISO form):
# 0, as many as the cores and the memory free allow (xfrecomp jobs, the
# recompile step's policy), else this many.
set(XF_GAME_JOBS 0 CACHE STRING "compilers at once for the generated code (0: by the cores and the memory free)")
if(NOT _XF_GAME_CPU_LEVEL_NAME)
    set(_XF_GAME_CPU_LEVEL_NAME ${XF_GAME_CPU_LEVEL})
endif()

# xenoforge_cpu_level_check(<target>): the CPU level check of an executable
# with statically linked generated code (xenoforge/cpu_level_check.h): a
# baseline-compiled file whose initializer runs before every other one and
# stops the program with a message on a CPU without the level the generated
# code was compiled for. A PREBUILT library is taken to be at XF_GAME_CPU_LEVEL.
function(xenoforge_cpu_level_check target)
    set(file ${CMAKE_CURRENT_BINARY_DIR}/xenoforge/cpu_level_check_${_XF_GAME_CPU_LEVEL_NAME}.cpp)
    file(WRITE ${file}.tmp "// written by xenoforge_add_port: the CPU level check (xenoforge/cpu_level_check.h)\n"
                           "#define XF_CPU_LEVEL_CHECK \"${_XF_GAME_CPU_LEVEL_NAME}\"\n"
                           "#include <xenoforge/cpu_level_check.h>\n")
    file(COPY_FILE ${file}.tmp ${file} ONLY_IF_DIFFERENT)
    target_sources(${target} PRIVATE ${file})
endfunction()

# xenoforge_shader_storage(<var> <title id>): where XenoForge keeps the
# shaders a title's runs recorded on this machine (game_build.h's
# default_cache_dir: %LOCALAPPDATA%/xenoforge/cache, $XDG_CACHE_HOME/xenoforge,
# ~/.cache/xenoforge), <cache>/<title id, 8 hex digits>.xfshaders, for
# xenoforge_add_port's SHADER_STORAGE.
function(xenoforge_shader_storage var title_id)
    string(TOUPPER "${title_id}" id)
    if(WIN32 AND DEFINED ENV{LOCALAPPDATA})
        file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/xenoforge/cache" cache)
    elseif(DEFINED ENV{XDG_CACHE_HOME})
        set(cache "$ENV{XDG_CACHE_HOME}/xenoforge")
    elseif(DEFINED ENV{HOME})
        set(cache "$ENV{HOME}/.cache/xenoforge")
    else()
        set(${var} "" PARENT_SCOPE)
        return()
    endif()
    set(${var} "${cache}/${id}.xfshaders" PARENT_SCOPE)
endfunction()

function(xenoforge_add_port name)
    cmake_parse_arguments(P "NO_PLAYER;NO_BOOT;HOT_RELOAD" "ISO;XEX;PROJECT;OPT;GENERATED_DIR;PREBUILT;SHADER_STORAGE"
                          "SOURCES;HOOK_SOURCES;DEFINITIONS;INCLUDES;LIBRARIES" ${ARGN})
    if(P_HOT_RELOAD AND NOT P_HOOK_SOURCES)
        message(FATAL_ERROR "xenoforge_add_port(${name}): HOT_RELOAD reloads HOOK_SOURCES, and there are none")
    endif()
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "xenoforge_add_port(${name}): unknown arguments ${P_UNPARSED_ARGUMENTS}")
    endif()
    set(opt ${P_OPT})
    if(NOT opt AND DEFINED XF_GAME_OPT_${name})
        set(opt ${XF_GAME_OPT_${name}})
    endif()
    if(NOT opt AND DEFINED XF_GAME_OPT)
        set(opt ${XF_GAME_OPT})
    endif()
    if(NOT opt)
        set(opt -O2)
    endif()

    # the game library
    if(P_PREBUILT)
        if(NOT EXISTS ${P_PREBUILT})
            message(FATAL_ERROR "xenoforge_add_port(${name}): no ${P_PREBUILT}")
        endif()
        add_library(game_${name} STATIC IMPORTED)
        set_target_properties(game_${name} PROPERTIES IMPORTED_LOCATION ${P_PREBUILT})
    elseif(P_GENERATED_DIR)
        if(NOT EXISTS ${P_GENERATED_DIR}/modules.cpp)
            message(FATAL_ERROR "xenoforge_add_port(${name}): no modules.cpp in ${P_GENERATED_DIR}: run xfrecomp emit "
                                "<iso> ${P_GENERATED_DIR} (an emission before the per-module tables has none)")
        endif()
        file(GLOB game_sources CONFIGURE_DEPENDS ${P_GENERATED_DIR}/modules.cpp ${P_GENERATED_DIR}/*/functions_*.cpp
             ${P_GENERATED_DIR}/*/table.cpp)
        add_library(game_${name} STATIC ${game_sources})
        target_include_directories(game_${name} PRIVATE ${XENOFORGE_INCLUDE_DIR})
        # optimized, without debug information (it is large) or warnings; never
        # -ffast-math (docs/recompiler.md)
        target_compile_options(game_${name} PRIVATE ${opt} -g0 -w ${XF_GAME_CPU_FLAGS})
    elseif(P_ISO)
        # The generated files are not known until xfrecomp has run, so the
        # emission and the compile are one step (build_game.cmake): xfrecomp
        # emit, a small project (game_library/) configured over what was
        # written (its glob sees it), and built with as many compilers as the
        # memory free allows (XF_GAME_JOBS); ninja inside recompiles only the
        # files whose text changed.
        get_filename_component(iso ${P_ISO} ABSOLUTE)
        if(XENOFORGE_HOST_XFRECOMP)
            set(xfrecomp ${XENOFORGE_HOST_XFRECOMP})
        elseif(CMAKE_CROSSCOMPILING AND NOT TARGET xfrecomp)
            message(FATAL_ERROR "xenoforge_add_port(${name}): cross-compiling (${CMAKE_SYSTEM_NAME} "
                                "${CMAKE_SYSTEM_PROCESSOR}), xfrecomp has to run on this machine: set "
                                "XENOFORGE_HOST_XFRECOMP to the xfrecomp of an SDK built for it")
        else()
            set(xfrecomp $<TARGET_FILE:XenoForge::xfrecomp>)
        endif()
        set(gen ${CMAKE_CURRENT_BINARY_DIR}/xenoforge/${name})
        set(lib ${gen}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}game_${name}${CMAKE_STATIC_LIBRARY_SUFFIX})
        set(depends ${iso})
        set(step_args)
        if(P_XEX)
            list(APPEND step_args -DXEX=${P_XEX})
        endif()
        if(P_PROJECT)
            get_filename_component(project ${P_PROJECT} ABSOLUTE)
            list(APPEND step_args -DPROJECT=${project})
            list(APPEND depends ${project})
        endif()
        # the port's target (ARM64 Linux, Android: a toolchain file) for the generated code too
        foreach(v TOOLCHAIN_FILE SYSROOT)
            if(CMAKE_${v})
                list(APPEND step_args -D${v}=${CMAKE_${v}})
            endif()
        endforeach()
        foreach(v ANDROID_ABI ANDROID_PLATFORM ANDROID_STL)
            if(${v})
                list(APPEND step_args -D${v}=${${v}})
            endif()
        endforeach()
        file(GLOB interface_headers CONFIGURE_DEPENDS ${XENOFORGE_INCLUDE_DIR}/xenoforge/ppc*.h
             ${XENOFORGE_INCLUDE_DIR}/xenoforge/ppc_ops/*.h ${XENOFORGE_INCLUDE_DIR}/xenoforge/ppc_opcodes.inc
             ${XENOFORGE_INCLUDE_DIR}/xenoforge/game_library.h ${XENOFORGE_INCLUDE_DIR}/xenoforge/recomp.h)
        # the flags as space-separated strings: one argument each, whatever splits lists on the way
        list(JOIN opt " " opt_text)
        list(JOIN XF_GAME_CPU_FLAGS " " cpu_flags_text)
        add_custom_command(
            OUTPUT ${lib}
            COMMAND ${CMAKE_COMMAND} -DXFRECOMP=${xfrecomp} -DISO=${iso} -DGEN=${gen} -DNAME=${name}
                    -DGAME_LIBRARY_DIR=${_XF_PORT_DIR}/game_library -DGENERATOR=${CMAKE_GENERATOR}
                    -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM} -DC_COMPILER=${CMAKE_C_COMPILER}
                    -DCXX_COMPILER=${CMAKE_CXX_COMPILER} -DINCLUDE_DIR=${XENOFORGE_INCLUDE_DIR} "-DOPT=${opt_text}"
                    "-DCPU_FLAGS=${cpu_flags_text}" -DJOBS=${XF_GAME_JOBS} ${step_args}
                    -P ${_XF_PORT_DIR}/build_game.cmake
            DEPENDS ${depends} ${interface_headers} ${_XF_PORT_DIR}/game_library/CMakeLists.txt
                    ${_XF_PORT_DIR}/build_game.cmake ${xfrecomp}
            COMMENT "${name}: xfrecomp emit, then the generated code"
            USES_TERMINAL
            VERBATIM)
        add_custom_target(game_${name}_build DEPENDS ${lib})
        add_library(game_${name} INTERFACE)
        target_link_libraries(game_${name} INTERFACE ${lib})
        add_dependencies(game_${name} game_${name}_build)
    else()
        message(FATAL_ERROR "xenoforge_add_port(${name}): one of ISO, GENERATED_DIR or PREBUILT is needed")
    endif()

    # the shader bake (native_shaders.h): what a run of the game recorded (XenoForge's shader storage,
    # <cache>/<title id>.xfshaders) translated into shaders/ beside the executables, where a port's
    # ShaderCache reads it, again whenever the storage grew
    if(P_SHADER_STORAGE)
        get_filename_component(storage ${P_SHADER_STORAGE} ABSOLUTE)
        set(xfshader)
        if(XENOFORGE_HOST_XFRECOMP)
            get_filename_component(host_bin ${XENOFORGE_HOST_XFRECOMP} DIRECTORY)
            set(xfshader ${host_bin}/xfshader${CMAKE_HOST_EXECUTABLE_SUFFIX})
        elseif(NOT CMAKE_CROSSCOMPILING OR TARGET xfshader)
            set(xfshader $<TARGET_FILE:XenoForge::xfshader>)
        endif()
        if(NOT EXISTS ${storage})
            message(STATUS "xenoforge_add_port(${name}): no ${storage} yet (a run of the game records it): no bake")
        elseif(NOT xfshader)
            message(STATUS "xenoforge_add_port(${name}): cross-compiling without XENOFORGE_HOST_XFRECOMP: no bake")
        else()
            set(baked ${CMAKE_CURRENT_BINARY_DIR}/xenoforge/${name}/shaders.baked)
            add_custom_command(OUTPUT ${baked}
                               COMMAND ${xfshader} bake ${storage} ${CMAKE_CURRENT_BINARY_DIR}/shaders
                               COMMAND ${CMAKE_COMMAND} -E touch ${baked}
                               DEPENDS ${storage} ${xfshader}
                               COMMENT "${name}: the shaders a run recorded, baked into shaders/"
                               VERBATIM)
            add_custom_target(${name}_shaders ALL DEPENDS ${baked})
        endif()
    endif()

    # the hooks as a module of their own, reloaded while the game runs
    if(P_HOT_RELOAD)
        set(entry ${CMAKE_CURRENT_BINARY_DIR}/xenoforge/${name}/port_module_entry.cpp)
        file(WRITE ${entry}.tmp "// written by xenoforge_add_port: the module's side of xenoforge/port_module.h\n"
                                "#define XF_PORT_MODULE_ENTRY\n#include <xenoforge/port_module.h>\n")
        file(COPY_FILE ${entry}.tmp ${entry} ONLY_IF_DIFFERENT)
        add_library(${name}_hooks SHARED ${P_HOOK_SOURCES} ${entry})
        target_compile_definitions(${name}_hooks PRIVATE XF_PORT_MODULE ${P_DEFINITIONS})
        target_include_directories(${name}_hooks PRIVATE ${XENOFORGE_INCLUDE_DIR} ${P_INCLUDES})
        target_compile_features(${name}_hooks PRIVATE cxx_std_20)
        set_target_properties(${name}_hooks PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
    endif()

    # the executables
    set(kinds)
    if(NOT P_NO_PLAYER AND TARGET XenoForge::player)
        list(APPEND kinds player)
    endif()
    if(NOT P_NO_BOOT)
        list(APPEND kinds boot)
    endif()
    foreach(kind ${kinds})
        set(sources ${P_SOURCES})
        if(NOT sources)
            # no code of its own: the stub main (tools/stub_main.cpp's twin, written here)
            set(stub ${CMAKE_CURRENT_BINARY_DIR}/xenoforge/${name}/${kind}_main.cpp)
            file(WRITE ${stub}.tmp "#include <xenoforge/run.h>\nint main(int argc, char **argv) {\n    xf::PortInfo port;\n    return xf::run(argc, argv, port);\n}\n")
            file(COPY_FILE ${stub}.tmp ${stub} ONLY_IF_DIFFERENT)
            set(sources ${stub})
        endif()
        if(NOT P_HOT_RELOAD)
            list(APPEND sources ${P_HOOK_SOURCES})
        endif()
        if(ANDROID AND kind STREQUAL player)
            # Android starts a game through SDL's Java activity (SDLActivity), which loads
            # lib<name>.so from the APK and calls the SDL_main it exports: the port's main
            # becomes that function (a C symbol), the player a shared library
            set(android_main ${CMAKE_CURRENT_BINARY_DIR}/xenoforge/${name}/android_main.h)
            file(WRITE ${android_main}.tmp "// written by xenoforge_add_port: the port's main as SDL_main on Android\n"
                                           "extern \"C\" __attribute__((visibility(\"default\"))) int SDL_main(int argc, char **argv);\n"
                                           "#define main SDL_main\n")
            file(COPY_FILE ${android_main}.tmp ${android_main} ONLY_IF_DIFFERENT)
            add_library(${kind}_${name} SHARED ${sources})
            target_compile_options(${kind}_${name} PRIVATE -include ${android_main})
        else()
            add_executable(${kind}_${name} ${sources})
        endif()
        xenoforge_cpu_level_check(${kind}_${name})
        target_compile_definitions(${kind}_${name} PRIVATE XF_PORT_LINKED ${P_DEFINITIONS})
        if(P_HOT_RELOAD)
            target_compile_definitions(${kind}_${name} PRIVATE "XF_PORT_HOOK_MODULE=\"$<TARGET_FILE_NAME:${name}_hooks>\"")
            add_dependencies(${kind}_${name} ${name}_hooks)
        endif()
        if(kind STREQUAL boot)
            target_compile_definitions(${kind}_${name} PRIVATE XF_PORT_BOOT)
        endif()
        target_include_directories(${kind}_${name} PRIVATE ${P_INCLUDES})
        target_link_libraries(${kind}_${name} PRIVATE XenoForge::${kind} XenoForge::kernel XenoForge::vk XenoForge::core
                              game_${name} ${P_LIBRARIES})
        if(TARGET game_${name}_build)
            add_dependencies(${kind}_${name} game_${name}_build)
        endif()
        foreach(file ${XENOFORGE_RUNTIME_FILES})
            add_custom_command(TARGET ${kind}_${name} POST_BUILD
                               COMMAND ${CMAKE_COMMAND} -E copy_if_different ${file} $<TARGET_FILE_DIR:${kind}_${name}>
                               VERBATIM)
        endforeach()
    endforeach()
endfunction()
