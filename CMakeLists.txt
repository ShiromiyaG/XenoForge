cmake_minimum_required(VERSION 3.25)
# optimized unless asked otherwise
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
project(xenoforge VERSION 0.1.0 LANGUAGES C CXX)
# the player's flat folder (package.cmake) is the default install component; the SDK package (install_sdk.cmake) is "sdk"
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME player)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra)
endif()
if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# The lean Xbox 360 system: so far the disc, the executable and the system's
# export tables (docs/plan.md, A1), and Xenia's GPU traces read and counted
# (B1).
add_library(xenoforge STATIC
    src/aes.cpp
    src/bignum.cpp
    src/des.cpp
    src/disc.cpp
    src/package.cpp
    src/package_write.cpp
    src/kernel/gpd.cpp
    src/profile_gpd.cpp
    src/exports.cpp
    src/xex.cpp
    src/lzx.cpp
    src/sha1.cpp
    src/sha2.cpp
    src/md5.cpp
    src/snappy.cpp
    src/gpu_trace.cpp
    src/game_build.cpp
    src/game_library.cpp
    src/shader_storage.cpp
    src/trace_stats.cpp
    src/ucode.cpp
    src/ucode_parse.cpp
    src/ucode_disasm.cpp
    src/xenos.cpp
    src/spirv.cpp
    src/shader_translator.cpp
    src/shader_interpreter.cpp
    src/shader_container.cpp
    src/texture.cpp
    src/resolve.cpp
    src/ppc.cpp
    src/ppc_interp.cpp
    src/ppc_runtime.cpp
    src/fuzz.cpp
    src/guest_space.cpp
    src/recomp_image.cpp
    src/recomp_discover.cpp
    src/recomp_emit.cpp
    src/recomp_module.cpp
    src/recomp_project.cpp
    src/toml.cpp
    src/hooks.cpp
    src/memory.cpp
    src/guest_memory.cpp
    src/gpu/command_processor.cpp
    src/gpu_trace_writer.cpp
    src/xma_decoder.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/game_interface.stamp
)
target_include_directories(xenoforge PUBLIC include PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
# The game library's interface (game_library.h; docs/runtime.md, "The game
# library"): its version, a digest of what makes a library (the generated
# code's headers, the recompiler's sources, the recompile step's flags in
# game_build.cpp), and those headers as text, which the recompile step writes
# beside the code it compiles (cmake/game_interface.cmake)
file(GLOB XF_GAME_INTERFACE_INPUTS CONFIGURE_DEPENDS include/xenoforge/ppc*.h include/xenoforge/ppc_ops/*.h
     include/xenoforge/ppc_opcodes.inc include/xenoforge/game_library.h include/xenoforge/recomp.h src/recomp_*.cpp
     src/game_build.cpp)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/game_interface.stamp
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/xenoforge/game_interface.h
               ${CMAKE_CURRENT_BINARY_DIR}/generated/game_headers.inc
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/game_interface.cmake
    DEPENDS cmake/game_interface.cmake ${XF_GAME_INTERFACE_INPUTS}
    COMMENT "the game library's interface version")

# XMA decoded by FFmpeg's xmaframes decoder, as Xenia does (plan A6;
# third_party/ffmpeg/ffmpeg.cmake): a shared library of its own (LGPL), fetched
# unless XF_FFMPEG_DIR or xenia-canary's checkout has it, and loaded at run time
# (src/xma_decoder.cpp): the core is compiled against FFmpeg's headers, never
# linked to it, so a program without the library still runs and decodes XMA as
# silence. Off, the core builds without FFmpeg at all.
option(XF_XMA "decode XMA with FFmpeg (a shared library, LGPL, loaded at run time)" ON)
if(XF_XMA)
    include(third_party/ffmpeg/ffmpeg.cmake)
    target_compile_definitions(xenoforge PRIVATE XF_XMA=1)
    target_include_directories(xenoforge SYSTEM PRIVATE $<TARGET_PROPERTY:ffmpeg_xma,INTERFACE_INCLUDE_DIRECTORIES>)
    add_dependencies(xenoforge ffmpeg_xma) # built beside the executables
    target_link_libraries(xenoforge PRIVATE ${CMAKE_DL_LIBS})
endif()

# Khronos's SPIR-V headers (MIT), from the Vulkan SDK; spirv-val checks what
# the translator writes in the tests
find_path(SPIRV_HEADERS_DIR spirv.hpp11 HINTS "$ENV{VULKAN_SDK}/Include/spirv-headers" REQUIRED)
target_include_directories(xenoforge PUBLIC ${SPIRV_HEADERS_DIR})
find_program(SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin")

# The parts that talk to a GPU, apart, so the rest builds without Vulkan:
# the compute runner of the shader checks, and the renderer (plan B3), whose
# rectangle list geometry shader glslc compiles to SPIR-V words it includes.
find_package(Vulkan REQUIRED)
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
set(XF_SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(XF_SHADER_WORDS)
# xf_shader(<name> <source> [defines...]): src/shaders/<source> to <name>.inc
function(xf_shader name source)
    add_custom_command(
        OUTPUT ${XF_SHADER_OUT}/${name}.inc
        COMMAND ${CMAKE_COMMAND} -E make_directory ${XF_SHADER_OUT}
        COMMAND ${GLSLC} --target-env=vulkan1.3 -mfmt=num ${ARGN} -o ${XF_SHADER_OUT}/${name}.inc
                ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${source}
        DEPENDS src/shaders/${source} src/shaders/edram.glsl src/shaders/tiling.glsl
        COMMENT "glslc ${name}")
    set(XF_SHADER_WORDS ${XF_SHADER_WORDS} ${XF_SHADER_OUT}/${name}.inc PARENT_SCOPE)
endfunction()
xf_shader(rectangle_list.geom rectangle_list.geom)
xf_shader(point_list.geom point_list.geom)
xf_shader(fullscreen.vert fullscreen.vert)
xf_shader(tessellation.vert tessellation.vert)
xf_shader(tessellation_1cp.tesc tessellation.tesc -DCONTROL_POINTS=1)
xf_shader(tessellation_3cp.tesc tessellation.tesc -DCONTROL_POINTS=3)
xf_shader(tessellation_4cp.tesc tessellation.tesc -DCONTROL_POINTS=4)
xf_shader(tessellation_adaptive_triangle.tesc tessellation.tesc -DADAPTIVE)
xf_shader(tessellation_adaptive_quad.tesc tessellation.tesc -DADAPTIVE -DQUAD)
xf_shader(edram_store.comp edram_store.comp)
xf_shader(edram_store_ms.comp edram_store.comp -DMS)
xf_shader(edram_load.frag edram_load.frag)
xf_shader(edram_load_uint.frag edram_load.frag -DUINT)
xf_shader(edram_load_depth.frag edram_load.frag -DDEPTH)
xf_shader(edram_load_stencil.frag edram_load.frag -DDEPTH -DSTENCIL)
xf_shader(edram_fsi_library.frag edram_fsi.frag -DLIBRARY)
xf_shader(edram_fsi_depth_pixel.frag edram_fsi.frag -DDEPTH_ONLY)
xf_shader(edram_fsi_depth_sample.frag edram_fsi.frag -DDEPTH_ONLY -DSAMPLE_INTERLOCK)
xf_shader(edram_clear.comp edram_clear.comp)
xf_shader(depth_float24.frag depth_float24.frag)
xf_shader(depth_float24_round.frag depth_float24.frag -DROUND)
xf_shader(placeholder.frag placeholder.frag)
xf_shader(resolve.comp resolve.comp)
xf_shader(texture_load.comp texture_load.comp)
xf_shader(texture_load_test.comp texture_load.comp -DPARAMS_BUFFER) # texture_gpu_test's
add_library(xenoforge_vk STATIC src/vk_compute.cpp src/vk_device.cpp src/renderer.cpp src/native_render.cpp src/replay.cpp src/shader_step.cpp ${XF_SHADER_WORDS})
target_include_directories(xenoforge_vk PRIVATE ${XF_SHADER_OUT})
target_link_libraries(xenoforge_vk PUBLIC xenoforge Vulkan::Vulkan)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # Vulkan's structures are filled as VkX info{VK_STRUCTURE_TYPE_X}, the rest zero
    target_compile_options(xenoforge_vk PRIVATE -Wno-missing-field-initializers)
endif()

add_executable(xfinspect tools/xfinspect.cpp)
target_link_libraries(xfinspect PRIVATE xenoforge)
add_executable(xftrace tools/xftrace.cpp)
target_link_libraries(xftrace PRIVATE xenoforge)
add_executable(xfshader tools/xfshader.cpp)
target_link_libraries(xfshader PRIVATE xenoforge)
add_executable(xfeffect tools/xfeffect.cpp)
target_link_libraries(xfeffect PRIVATE xenoforge)
add_executable(xfppc tools/xfppc.cpp)
target_link_libraries(xfppc PRIVATE xenoforge)
add_executable(xfrecomp tools/xfrecomp.cpp)
target_link_libraries(xfrecomp PRIVATE xenoforge)
# the recompile step from the command line (game_build.h): a disc into the cache, timed
add_executable(xfbuild tools/xfbuild.cpp)
target_link_libraries(xfbuild PRIVATE xenoforge_vk xenoforge)

# The kernel (step A4; docs/runtime.md): an object library, so every
# export's registration (XF_EXPORT, a static initializer) is linked in.
file(GLOB XF_KERNEL_SOURCES CONFIGURE_DEPENDS src/kernel/*.cpp)
list(REMOVE_ITEM XF_KERNEL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/kernel/gpd.cpp) # (in the core: the launcher reads GPDs too)
add_library(xenoforge_kernel OBJECT ${XF_KERNEL_SOURCES})
target_link_libraries(xenoforge_kernel PUBLIC xenoforge ${CMAKE_DL_LIBS}) # (dladdr and dlopen on POSIX: crash.cpp, port_module.cpp)
# the console's own XAM (the player's system update) as the oracle for ours
add_executable(xfxam tools/xfxam.cpp)
target_include_directories(xfxam PRIVATE src/kernel)
target_link_libraries(xfxam PRIVATE xenoforge_kernel xenoforge)
# xfxam on the console's XAM where XF_XAM_XEX names it (skipped otherwise)
add_executable(xam_oracle_test tests/xam_oracle_test.cpp)
add_test(NAME xam_oracle COMMAND xam_oracle_test $<TARGET_FILE:xfxam>)
set_tests_properties(xam_oracle PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
# boot linked with a game's generated code: `xfrecomp emit <iso> <XF_GEN_DIR>/<name>`
# writes it, a directory per executable and DLL on the disc and modules.cpp
# listing their tables (game code: outside the repository); -DXF_GAMES="<name>;<name>"
# builds boot_<name> for each, with one game library carrying every module. The
# code is built here, against these headers, so it follows every change to the
# instructions' semantics. XF_GAME_OPT is the game libraries' optimization
# (-O1 builds in about 70% of -O2's time and runs slower; docs/recompiler.md,
# "Modules and the game library"), XF_GAME_OPT_<name> one game's.
set(XF_GAMES "" CACHE STRING "games whose generated code a boot_<name> links (directories under XF_GEN_DIR)")
set(XF_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/gen-modules" CACHE PATH "where xfrecomp emit wrote each game")
set(XF_GAME_OPT "-O2" CACHE STRING "the game libraries' optimization level")
# XF_GAME_LIBS: a directory of game_<name>.lib built elsewhere (another build
# of the same headers), linked instead of compiling the game's code again. For
# work that leaves the instructions' semantics and the generated code's
# headers alone (ppc_context.h, ppc_ops*, ppc_generated.h, ppc_module.h): a
# build that changes them must compile the game libraries itself.
set(XF_GAME_LIBS "" CACHE PATH "prebuilt game_<name>.lib to link instead of compiling the games")
# the kernel's threads and objects, tested through their exports and
# src/kernel/objects.h
add_executable(kernel_threads_test tests/kernel_threads_test.cpp)
target_include_directories(kernel_threads_test PRIVATE src/kernel)
target_link_libraries(kernel_threads_test PRIVATE xenoforge_kernel xenoforge)
# the same on the deterministic mode's scheduler, and that mode's own test
# (src/kernel/deterministic.cpp, input recordings)
add_executable(kernel_threads_deterministic_test tests/kernel_threads_test.cpp)
target_compile_definitions(kernel_threads_deterministic_test PRIVATE XF_DETERMINISTIC_TEST)
target_include_directories(kernel_threads_deterministic_test PRIVATE src/kernel)
target_link_libraries(kernel_threads_deterministic_test PRIVATE xenoforge_kernel xenoforge)
add_executable(kernel_deterministic_test tests/kernel_deterministic_test.cpp)
target_include_directories(kernel_deterministic_test PRIVATE src/kernel)
target_link_libraries(kernel_deterministic_test PRIVATE xenoforge_kernel xenoforge)
# crash and stop reports: a thread's report, the interpreter's stop, and real
# faults in child processes (include/xenoforge/crash.h)
add_executable(kernel_crash_test tests/kernel_crash_test.cpp)
target_include_directories(kernel_crash_test PRIVATE src/kernel)
target_link_libraries(kernel_crash_test PRIVATE xenoforge_kernel xenoforge)
# the exception dispatch on a guest program (tests/seh_program.h), interpreted
# and recompiled: seh_gen emits the program's functions as xfrecomp would
add_executable(seh_gen tests/seh_gen.cpp)
target_link_libraries(seh_gen PRIVATE xenoforge)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/seh_generated.cpp
    COMMAND seh_gen ${CMAKE_CURRENT_BINARY_DIR}/seh_generated.cpp
    DEPENDS seh_gen
    COMMENT "seh_gen")
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/seh_generated.cpp PROPERTIES COMPILE_OPTIONS "-O1;-g0")
add_executable(kernel_exceptions_test tests/kernel_exceptions_test.cpp ${CMAKE_CURRENT_BINARY_DIR}/seh_generated.cpp)
target_include_directories(kernel_exceptions_test PRIVATE src/kernel tests)
target_link_libraries(kernel_exceptions_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_exceptions COMMAND kernel_exceptions_test)
set_tests_properties(kernel_exceptions PROPERTIES TIMEOUT 120)
# the port's side (docs/recompiler.md, "The project file", "Replacing and
# hooking"): the project file and the hooks on a guest program port_gen emits
# with its project file, run generated and interpreted
add_executable(port_gen tests/port_gen.cpp)
target_link_libraries(port_gen PRIVATE xenoforge)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/port_gen/m/functions_0.cpp ${CMAKE_CURRENT_BINARY_DIR}/port_gen/m/table.cpp
    COMMAND port_gen ${CMAKE_CURRENT_BINARY_DIR}/port_gen
    DEPENDS port_gen
    COMMENT "port_gen")
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/port_gen/m/functions_0.cpp
                            ${CMAKE_CURRENT_BINARY_DIR}/port_gen/m/table.cpp PROPERTIES COMPILE_OPTIONS "-O1;-g0;-w")
add_executable(port_test tests/port_test.cpp ${CMAKE_CURRENT_BINARY_DIR}/port_gen/m/functions_0.cpp
                         ${CMAKE_CURRENT_BINARY_DIR}/port_gen/m/table.cpp)
target_include_directories(port_test PRIVATE tests)
target_link_libraries(port_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME port COMMAND port_test)
set_tests_properties(port PROPERTIES TIMEOUT 120)
# debugging for porters (plan E5): the hook frames, the counts, the owners,
# the guest paused, and a hook module (a DLL built as xenoforge_add_port's
# HOT_RELOAD builds one) loaded and reloaded
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/port_test_hooks_entry.cpp.tmp
     "#define XF_PORT_MODULE_ENTRY\n#include <xenoforge/port_module.h>\n")
file(COPY_FILE ${CMAKE_CURRENT_BINARY_DIR}/port_test_hooks_entry.cpp.tmp
     ${CMAKE_CURRENT_BINARY_DIR}/port_test_hooks_entry.cpp ONLY_IF_DIFFERENT)
add_library(port_test_hooks SHARED tests/port_module_hooks.cpp ${CMAKE_CURRENT_BINARY_DIR}/port_test_hooks_entry.cpp)
target_compile_definitions(port_test_hooks PRIVATE XF_PORT_MODULE)
target_include_directories(port_test_hooks PRIVATE include tests)
set_target_properties(port_test_hooks PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
add_executable(port_debug_test tests/port_debug_test.cpp)
target_include_directories(port_debug_test PRIVATE tests src/kernel)
target_link_libraries(port_debug_test PRIVATE xenoforge_kernel xenoforge)
target_compile_definitions(port_debug_test PRIVATE "XF_TEST_HOOK_MODULE=\"$<TARGET_FILE:port_test_hooks>\""
                                                   "XF_TEST_NOT_A_MODULE=\"$<TARGET_FILE:port_gen>\"")
add_dependencies(port_debug_test port_test_hooks port_gen)
add_test(NAME port_debug COMMAND port_debug_test)
set_tests_properties(port_debug PROPERTIES TIMEOUT 120)

# stb_image and stb_image_write (public domain / MIT), for PNGs in the tools;
# Xenia's copy unless STB_DIR says otherwise
if(NOT TARGET stb)
    set(STB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xenia-canary/third_party/stb" CACHE PATH "stb's headers")
    add_library(stb INTERFACE)
    target_include_directories(stb SYSTEM INTERFACE ${STB_DIR})
endif()
add_executable(xfreplay tools/xfreplay.cpp)
target_link_libraries(xfreplay PRIVATE xenoforge_vk stb)
add_executable(xfcompare tools/xfcompare.cpp)
target_link_libraries(xfcompare PRIVATE stb)
# boot as a library (a port's boot_<name> links it: tools/boot.cpp's run_boot, the shared tools/game.cpp) and the
# generic boot: the game library cached for the disc, else the interpreter. --render draws what the GPU is sent (the
# renderer as the command processor's Sink) and writes shots
add_library(xenoforge_boot STATIC tools/boot.cpp tools/game.cpp tools/sampler.cpp)
target_link_libraries(xenoforge_boot PUBLIC xenoforge_vk xenoforge PRIVATE stb)
add_executable(boot tools/stub_main.cpp)
target_compile_definitions(boot PRIVATE XF_PORT_BOOT)
target_link_libraries(boot PRIVATE xenoforge_boot xenoforge_kernel xenoforge_vk xenoforge)
# The player (track C1; docs/runtime.md, "The player"): a game in an SDL3
# window with its input and sound, the overlay in Dear ImGui, the settings in
# config.toml. SDL3, Dear ImGui and toml++ are fetched as the 3DS runtime pins
# them, only when the player is built (by default with XF_GAMES), so the core
# and its tests build without them. `player` runs a game interpreted;
# player_<game> links its generated code as boot_<game> does.
if(XF_GAMES)
    set(XF_PLAYER_DEFAULT ON)
else()
    set(XF_PLAYER_DEFAULT OFF)
endif()
option(XF_PLAYER "build the player (fetches SDL3, Dear ImGui, toml++)" ${XF_PLAYER_DEFAULT})
if(XF_PLAYER AND TARGET boot)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.16 GIT_SHALLOW TRUE)
    FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0
                         GIT_SHALLOW TRUE)
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9 GIT_SHALLOW TRUE)
    # RmlUi (MIT) with FreeType (its FTL; no zlib, PNG, HarfBuzz or Brotli),
    # for what the player's user sees: the launcher (docs/runtime.md, "The
    # launcher"). RmlUi finds FreeType by the Freetype::Freetype target.
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
    set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
    set(RMLUI_BACKEND "auto" CACHE STRING "" FORCE)
    FetchContent_Declare(freetype GIT_REPOSITORY https://github.com/freetype/freetype.git GIT_TAG VER-2-14-3
                         GIT_SHALLOW TRUE)
    FetchContent_Declare(RmlUi GIT_REPOSITORY https://github.com/mikke89/RmlUi.git GIT_TAG 6.3 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3 tomlplusplus imgui freetype)
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()
    set(_shared ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(RmlUi)
    set(BUILD_SHARED_LIBS ${_shared})
    target_compile_options(freetype PRIVATE -w)
    target_compile_options(rmlui_core PRIVATE -w)
    target_compile_options(SDL3-static PRIVATE -w) # the project's -Wall is not for SDL's sources
    add_library(xf_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
    target_include_directories(xf_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(xf_imgui PUBLIC SDL3::SDL3-static Vulkan::Vulkan)
    target_compile_options(xf_imgui PRIVATE -w)
    # the window, input, sound and settings, apart from the kernel they feed;
    # the window's front buffer conversion, a compute shader glslc compiles
    add_custom_command(
        OUTPUT ${XF_SHADER_OUT}/front_buffer.comp.inc
        COMMAND ${CMAKE_COMMAND} -E make_directory ${XF_SHADER_OUT}
        COMMAND ${GLSLC} --target-env=vulkan1.3 -mfmt=num -o ${XF_SHADER_OUT}/front_buffer.comp.inc
                ${CMAKE_CURRENT_SOURCE_DIR}/frontend/front_buffer.comp
        DEPENDS frontend/front_buffer.comp
        COMMENT "glslc front_buffer.comp")
    add_library(xf_frontend STATIC frontend/settings.cpp frontend/presenter.cpp frontend/sdl_input.cpp
                                   frontend/profiles.cpp frontend/console_options.cpp frontend/system_ui.cpp
                                   frontend/sdl_audio.cpp frontend/library.cpp frontend/launcher_core.cpp
                                   ${XF_SHADER_OUT}/front_buffer.comp.inc)
    target_include_directories(xf_frontend PRIVATE ${XF_SHADER_OUT})
    target_link_libraries(xf_frontend PUBLIC xenoforge xenoforge_vk SDL3::SDL3-static Vulkan::Vulkan
                                             tomlplusplus::tomlplusplus)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(xf_frontend PRIVATE -Wno-missing-field-initializers)
    endif()
    # the player as a library (frontend/player.cpp's run_player; a port's player_<name> links it) and the
    # generic player, which carries no game code
    add_library(xenoforge_player STATIC frontend/player.cpp frontend/system_ui_view.cpp frontend/overlay_system.cpp
                                        tools/game.cpp)
    target_link_libraries(xenoforge_player PUBLIC xf_frontend xf_imgui xenoforge_vk xenoforge PRIVATE stb)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(xenoforge_player PRIVATE -Wno-missing-field-initializers)
    endif()
    add_executable(player tools/stub_main.cpp)
    target_link_libraries(player PRIVATE xenoforge_player xenoforge_kernel xenoforge_vk xenoforge)
    add_library(XenoForge::player ALIAS xenoforge_player)
    # the launcher (track C2; docs/runtime.md, "The launcher"): the games in a
    # folder, their settings and controls, and each started in the player
    # beside it that carries its code; a window program, its form drawn by
    # SDL's renderer
    # The view is RmlUi (frontend/ui): its Vulkan render interface on the
    # frontend's own vk::Device, the screens written as RML and RCSS beside
    # the executable (ui/), and the font (Inter, SIL OFL) with them.
    foreach(shader vert frag)
        add_custom_command(
            OUTPUT ${XF_SHADER_OUT}/ui.${shader}.inc
            COMMAND ${CMAKE_COMMAND} -E make_directory ${XF_SHADER_OUT}
            COMMAND ${GLSLC} --target-env=vulkan1.3 -mfmt=num -o ${XF_SHADER_OUT}/ui.${shader}.inc
                    ${CMAKE_CURRENT_SOURCE_DIR}/frontend/ui/ui.${shader}
            DEPENDS frontend/ui/ui.${shader} frontend/ui/ui_draw.glsl
            COMMENT "glslc ui.${shader}")
    endforeach()
    add_library(xf_ui STATIC frontend/ui/ui_renderer.cpp frontend/ui/ui_system.cpp frontend/ui/ui_window.cpp
                             frontend/prepare_job.cpp frontend/stale_library.cpp frontend/launcher_model.cpp frontend/launcher_html.cpp
                             frontend/launcher_view.cpp frontend/launcher_screens.cpp frontend/launcher_input.cpp
                             frontend/settings_view.cpp
                             ${XF_SHADER_OUT}/ui.vert.inc ${XF_SHADER_OUT}/ui.frag.inc)
    target_include_directories(xf_ui PRIVATE ${XF_SHADER_OUT})
    target_include_directories(xf_ui PUBLIC frontend)
    target_link_libraries(xf_ui PUBLIC xf_frontend RmlUi::Core stb)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(xf_ui PRIVATE -Wno-missing-field-initializers)
    endif()
    add_executable(launcher WIN32 frontend/launcher.cpp)
    target_link_libraries(launcher PRIVATE xf_ui xf_frontend stb)
    set(XF_UI_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/frontend/ui)
    add_custom_command(TARGET launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${XF_UI_ASSETS}/assets $<TARGET_FILE_DIR:launcher>/ui
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${XF_UI_ASSETS}/fonts $<TARGET_FILE_DIR:launcher>/ui/fonts
        COMMENT "the launcher's windows, fonts and icons")
    install(DIRECTORY ${XF_UI_ASSETS}/assets/ DESTINATION ui)
    install(DIRECTORY ${XF_UI_ASSETS}/fonts/ DESTINATION ui/fonts)
    # the settings file, the sound's fold to stereo, the scripted presses
    add_executable(frontend_test tests/frontend_test.cpp tools/game.cpp)
    target_link_libraries(frontend_test PRIVATE xf_ui xf_frontend xenoforge_kernel xenoforge_vk)
    add_test(NAME frontend COMMAND frontend_test)
    # the player's system UI without a window: the dialogs' model, the profiles, the kernel's exports through the handler
    add_executable(system_ui_test tests/system_ui_test.cpp tools/game.cpp)
    target_include_directories(system_ui_test PRIVATE src/kernel)
    target_link_libraries(system_ui_test PRIVATE xf_frontend xenoforge_kernel xenoforge_vk xenoforge)
    add_test(NAME system_ui COMMAND system_ui_test)
    # the batch compatibility run (tools/xfcompat.cpp): the discs of a folder prepared and booted, a row each.
    # It reads the discs with the launcher's library and runs boot with SDL3's process calls, so it is built
    # with the player (which brings SDL3); its rows and the parser of boot's output are compat_test's.
    add_executable(xfcompat tools/xfcompat.cpp tools/compat.cpp)
    target_include_directories(xfcompat PRIVATE tools frontend)
    target_link_libraries(xfcompat PRIVATE xf_frontend xenoforge)
    add_dependencies(xfcompat boot)
endif()

# The games of XF_GAMES through the same function a port uses (cmake/xenoforge_port.cmake): game_<name>,
# boot_<name> and, with the player, player_<name>
add_library(XenoForge::core ALIAS xenoforge)
add_library(XenoForge::vk ALIAS xenoforge_vk)
add_library(XenoForge::kernel ALIAS xenoforge_kernel)
add_library(XenoForge::boot ALIAS xenoforge_boot)
add_executable(XenoForge::xfrecomp ALIAS xfrecomp)
set(XENOFORGE_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(XENOFORGE_RUNTIME_FILES)
if(XF_XMA)
    set(XENOFORGE_RUNTIME_FILES $<TARGET_FILE:ffmpeg_xma>)
endif()
include(cmake/xenoforge_port.cmake)
set(XF_PLAYERS)
if(TARGET player)
    set(XF_PLAYERS player)
endif()
# XF_GAME_HOOKS_<name>: hook sources (xenoforge/port.h, kept outside the
# repository) tried on that game as a port's would be, built as a module
# reloaded while it runs (HOT_RELOAD; docs/sdk.md, "Reloading the hooks")
foreach(game ${XF_GAMES})
    set(port_hooks)
    if(XF_GAME_HOOKS_${game})
        set(port_hooks HOOK_SOURCES ${XF_GAME_HOOKS_${game}} HOT_RELOAD)
    endif()
    if(XF_GAME_LIBS)
        xenoforge_add_port(${game} PREBUILT ${XF_GAME_LIBS}/game_${game}${CMAKE_STATIC_LIBRARY_SUFFIX} ${port_hooks})
    else()
        xenoforge_add_port(${game} GENERATED_DIR ${XF_GEN_DIR}/${game} ${port_hooks})
    endif()
    if(TARGET player_${game})
        list(APPEND XF_PLAYERS player_${game})
    endif()
endforeach()
# examples/native_port (a port drawing its own frames, docs/sdk.md "Native rendering") built here on one of XF_GAMES'
# generated code: player_native and boot_native in build/examples/native_port
set(XF_NATIVE_PORT_GAME "" CACHE STRING "a game of XF_GAMES to build examples/native_port on")
if(XF_NATIVE_PORT_GAME)
    if(XF_GAME_LIBS)
        set(NATIVE_PREBUILT ${XF_GAME_LIBS}/game_${XF_NATIVE_PORT_GAME}${CMAKE_STATIC_LIBRARY_SUFFIX})
    else()
        set(NATIVE_GENERATED_DIR ${XF_GEN_DIR}/${XF_NATIVE_PORT_GAME})
    endif()
    add_subdirectory(examples/native_port)
endif()
if(TARGET player)
    # the player as it is shipped: cmake --install, or the player_package target
    include(cmake/package.cmake)
endif()
# the SDK: find_package(XenoForge) in a port's own repository (docs/sdk.md)
include(cmake/install_sdk.cmake)
add_executable(xftexture tools/xftexture.cpp)
target_link_libraries(xftexture PRIVATE xenoforge stb)

enable_testing()
foreach(name aes disc xex lzx sha1 snappy ucode ucode_parse trace shader_interpreter texture ppc memory command_processor
             recomp_discover gpu_trace_writer shader_storage)
    add_executable(${name}_test tests/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE xenoforge)
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()
# xfcompat's rows, files and the parser of boot's output, on canned output
add_executable(compat_test tests/compat_test.cpp tools/compat.cpp)
target_include_directories(compat_test PRIVATE tools)
add_test(NAME compat COMMAND compat_test)
# the kernel's exports, called through a Kernel (the object library brings
# their registrations)
foreach(name kernel_memory kernel_modules kernel_crypt lockstep)
    add_executable(${name}_test tests/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE xenoforge_kernel xenoforge)
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()
# a game library built by the recompile step from a piece of generated code,
# loaded and run beside the same code linked in (needs clang and lld)
add_executable(game_library_test tests/game_library_test.cpp tests/game_library/code.cpp)
target_link_libraries(game_library_test PRIVATE xenoforge)
target_compile_definitions(game_library_test PRIVATE
                           XF_GAME_LIBRARY_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/game_library")
set_source_files_properties(tests/game_library/code.cpp PROPERTIES COMPILE_OPTIONS "-O2;-g0;-w;-mfma;-mavx2")
add_test(NAME game_library COMMAND game_library_test)
set_tests_properties(game_library PROPERTIES TIMEOUT 300)
# the recompile step on an executable the test makes: functions failing the
# emission and clang left out, the entry log taken in (needs clang and lld)
add_executable(game_build_test tests/game_build_test.cpp)
target_link_libraries(game_build_test PRIVATE xenoforge)
add_test(NAME game_build COMMAND game_build_test)
set_tests_properties(game_build PROPERTIES TIMEOUT 300)
# the modules family loads DLLs and runs a guest thread through them
target_include_directories(kernel_modules_test PRIVATE src/kernel)
set_tests_properties(kernel_modules PROPERTIES TIMEOUT 120) # threads: fail, never hang
foreach(name spirv shader_translator)
    add_executable(${name}_test tests/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE xenoforge)
    if(SPIRV_VAL)
        target_compile_definitions(${name}_test PRIVATE XF_SPIRV_VAL="${SPIRV_VAL}")
    endif()
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()
# the interlock path's translated pixel shaders, with edram_fsi.frag's library
target_sources(shader_translator_test PRIVATE ${XF_SHADER_OUT}/edram_fsi_library.frag.inc)
target_compile_definitions(shader_translator_test PRIVATE
                           XF_EDRAM_FSI_LIBRARY="${XF_SHADER_OUT}/edram_fsi_library.frag.inc")
add_test(NAME kernel_threads COMMAND kernel_threads_test)
add_test(NAME kernel_crash COMMAND kernel_crash_test)
# a rare deadlock seen once under load (2026-11-05, not reproduced in 36
# runs since): fail instead of hanging the suite
set_tests_properties(kernel_threads PROPERTIES TIMEOUT 120)
add_test(NAME kernel_threads_deterministic COMMAND kernel_threads_deterministic_test)
add_test(NAME kernel_deterministic COMMAND kernel_deterministic_test)
set_tests_properties(kernel_threads_deterministic kernel_deterministic PROPERTIES TIMEOUT 120)
# the video family: the GPU live through its exports, MMIO and threads
add_executable(kernel_video_test tests/kernel_video_test.cpp)
target_include_directories(kernel_video_test PRIVATE src/kernel)
target_link_libraries(kernel_video_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_video COMMAND kernel_video_test)
set_tests_properties(kernel_video PROPERTIES TIMEOUT 120) # threads: fail, never hang
# the content packages (STFS, SVOD) and their devices, on packages built in the test
add_executable(package_test tests/package_test.cpp)
target_include_directories(package_test PRIVATE src/kernel)
target_link_libraries(package_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME package COMMAND package_test)
set_tests_properties(package PROPERTIES TIMEOUT 120)
# writing STFS packages and reading them back
add_executable(package_write_test tests/package_write_test.cpp)
target_link_libraries(package_write_test PRIVATE xenoforge)
add_test(NAME package_write COMMAND package_write_test)
set_tests_properties(package_write PROPERTIES TIMEOUT 300)
# the files family and XAM, through their exports on a guest thread
add_executable(kernel_files_test tests/kernel_files_test.cpp)
target_include_directories(kernel_files_test PRIVATE src/kernel)
target_link_libraries(kernel_files_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_files COMMAND kernel_files_test)
set_tests_properties(kernel_files PROPERTIES TIMEOUT 120) # threads: fail, never hang
# XAM's network offline: sockets on guest threads
add_executable(kernel_net_test tests/kernel_net_test.cpp)
target_include_directories(kernel_net_test PRIVATE src/kernel)
target_link_libraries(kernel_net_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_net COMMAND kernel_net_test)
set_tests_properties(kernel_net PROPERTIES TIMEOUT 120) # threads: fail, never hang
# XAM's system families: content, devices, the dashboard, avatars, Kinect, tiles
add_executable(kernel_xam_system_test tests/kernel_xam_system_test.cpp)
target_include_directories(kernel_xam_system_test PRIVATE src/kernel)
target_link_libraries(kernel_xam_system_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_xam_system COMMAND kernel_xam_system_test)
set_tests_properties(kernel_xam_system PROPERTIES TIMEOUT 120) # threads: fail, never hang

# XAM's tasks: the worker pool, the handles
add_executable(kernel_xam_task_test tests/kernel_xam_task_test.cpp)
target_include_directories(kernel_xam_task_test PRIVATE src/kernel)
target_link_libraries(kernel_xam_task_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_xam_task COMMAND kernel_xam_task_test)
set_tests_properties(kernel_xam_task PROPERTIES TIMEOUT 120) # threads: fail, never hang

# XAM's users, profiles, locale and Win32 functions, on guest threads
add_executable(kernel_xam_user_test tests/kernel_xam_user_test.cpp)
target_include_directories(kernel_xam_user_test PRIVATE src/kernel)
target_link_libraries(kernel_xam_user_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_xam_user COMMAND kernel_xam_user_test)
set_tests_properties(kernel_xam_user PROPERTIES TIMEOUT 120) # threads: fail, never hang

# The system track: GPDs, a profile's settings and achievements kept across runs, the console's
# settings, the title ending (launch, terminate)
add_executable(kernel_system_test tests/kernel_system_test.cpp)
target_include_directories(kernel_system_test PRIVATE src/kernel)
target_link_libraries(kernel_system_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_system COMMAND kernel_system_test)
set_tests_properties(kernel_system PROPERTIES TIMEOUT 120) # threads: fail, never hang

# the exports ported from Xenia's inventory: locks, events, APCs, threads,
# Ob, the HAL, Rtl, the wide printf family, xbdm; some on guest threads
add_executable(kernel_inventory_test tests/kernel_inventory_test.cpp)
target_include_directories(kernel_inventory_test PRIVATE src/kernel)
target_link_libraries(kernel_inventory_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_inventory COMMAND kernel_inventory_test)
set_tests_properties(kernel_inventory PROPERTIES TIMEOUT 120) # threads: fail, never hang

# the object manager's guest-made objects (ObCreateObject + ObInsertObject) and the device objects
add_executable(kernel_ob_test tests/kernel_ob_test.cpp)
target_include_directories(kernel_ob_test PRIVATE src/kernel)
target_link_libraries(kernel_ob_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_ob COMMAND kernel_ob_test)
set_tests_properties(kernel_ob PROPERTIES TIMEOUT 120) # threads: fail, never hang

# XAM's system UI with no dialog drawn: defaults, overlapped, notifications
add_executable(kernel_ui_test tests/kernel_ui_test.cpp)
target_include_directories(kernel_ui_test PRIVATE src/kernel)
target_link_libraries(kernel_ui_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_ui COMMAND kernel_ui_test)
set_tests_properties(kernel_ui PROPERTIES TIMEOUT 120)
# the sound family: render driver clients and XMA contexts on guest threads
add_executable(kernel_audio_test tests/kernel_audio_test.cpp)
target_include_directories(kernel_audio_test PRIVATE src/kernel)
target_link_libraries(kernel_audio_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_audio COMMAND kernel_audio_test)
set_tests_properties(kernel_audio PROPERTIES TIMEOUT 120) # threads: fail, never hang
# the XMA decoder against Xenia's on recorded game input, where XF_XMA_TRACES says
add_executable(xma_replay_test tests/xma_replay_test.cpp)
target_include_directories(xma_replay_test PRIVATE src/kernel)
target_link_libraries(xma_replay_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME xma_replay COMMAND xma_replay_test)
set_tests_properties(xma_replay PROPERTIES TIMEOUT 300)
add_executable(vk_compute_test tests/vk_compute_test.cpp)
target_link_libraries(vk_compute_test PRIVATE xenoforge_vk)
add_test(NAME vk_compute COMMAND vk_compute_test)
# texture_load.comp against the CPU's decode, every format and fallback
add_executable(texture_gpu_test tests/texture_gpu_test.cpp)
target_include_directories(texture_gpu_test PRIVATE ${XF_SHADER_OUT})
target_link_libraries(texture_gpu_test PRIVATE xenoforge_vk)
add_test(NAME texture_gpu COMMAND texture_gpu_test)
set_tests_properties(texture_gpu PROPERTIES TIMEOUT 300)
# Xenia's PPC instruction tests recompiled by xfrecomp (when XF_PPC_TESTS_DIR
# names them, assembled by tools/assemble_ppc_tests.sh) and run as ppc_test
# runs them through the interpreter
set(XF_PPC_TESTS_DIR "" CACHE PATH "Xenia's PPC instruction tests, assembled")
if(XF_PPC_TESTS_DIR)
    set(XF_PPC_TESTS_OUT ${CMAKE_CURRENT_BINARY_DIR}/ppc_tests)
    set(XF_PPC_TESTS_SOURCES ${XF_PPC_TESTS_OUT}/ppc_tests_table.cpp)
    foreach(n RANGE 15)
        list(APPEND XF_PPC_TESTS_SOURCES ${XF_PPC_TESTS_OUT}/ppc_tests_${n}.cpp)
    endforeach()
    add_custom_command(
        OUTPUT ${XF_PPC_TESTS_SOURCES}
        COMMAND xfrecomp tests ${XF_PPC_TESTS_DIR} ${XF_PPC_TESTS_OUT} --files 16
        DEPENDS xfrecomp
        COMMENT "xfrecomp tests")
    # generated code: optimized, without debug information (it is large)
    set_source_files_properties(${XF_PPC_TESTS_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1;-g0")
    add_executable(ppc_recompiled_test tests/ppc_test.cpp ${XF_PPC_TESTS_SOURCES})
    target_compile_definitions(ppc_recompiled_test PRIVATE XF_PPC_RECOMPILED)
    target_link_libraries(ppc_recompiled_test PRIVATE xenoforge)
    add_test(NAME ppc_recompiled COMMAND ppc_recompiled_test)
    set_tests_properties(ppc_recompiled PROPERTIES ENVIRONMENT "XF_PPC_TESTS=${XF_PPC_TESTS_DIR}")
endif()
# The differential fuzzer (docs/recompiler.md, "The differential fuzzer"):
# xffuzz makes the cases (a fixed seed) and recompiles them; ppc_fuzz_test runs
# each through the interpreter and its generated code, built as the games' is
add_executable(xffuzz tools/xffuzz.cpp)
target_link_libraries(xffuzz PRIVATE xenoforge)
set(XF_FUZZ_OUT ${CMAKE_CURRENT_BINARY_DIR}/fuzz)
set(XF_FUZZ_SOURCES ${XF_FUZZ_OUT}/fuzz_table.cpp)
foreach(n RANGE 7)
    list(APPEND XF_FUZZ_SOURCES ${XF_FUZZ_OUT}/fuzz_${n}.cpp)
endforeach()
add_custom_command(
    OUTPUT ${XF_FUZZ_OUT}/cases.xfz ${XF_FUZZ_SOURCES}
    COMMAND xffuzz gen ${XF_FUZZ_OUT}/cases.xfz --seed 1 --per-op 12
    COMMAND xffuzz emit ${XF_FUZZ_OUT}/cases.xfz ${XF_FUZZ_OUT} --files 8
    DEPENDS xffuzz
    COMMENT "xffuzz gen and emit")
set_source_files_properties(${XF_FUZZ_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-g0;-w;-mfma;-mavx2")
add_executable(ppc_fuzz_test tests/ppc_fuzz_test.cpp ${XF_FUZZ_SOURCES})
target_compile_definitions(ppc_fuzz_test PRIVATE XF_FUZZ_CASES="${XF_FUZZ_OUT}/cases.xfz")
target_link_libraries(ppc_fuzz_test PRIVATE xenoforge)
add_test(NAME ppc_fuzz COMMAND ppc_fuzz_test)
add_executable(shader_compare_test tests/shader_compare_test.cpp)
target_link_libraries(shader_compare_test PRIVATE xenoforge_vk)
add_test(NAME shader_compare COMMAND shader_compare_test)
# the renderer against Xenia's pictures, where XF_TRACES and XF_DUMPS say
add_executable(replay_test tests/replay_test.cpp)
target_link_libraries(replay_test PRIVATE xenoforge_vk stb)
add_test(NAME replay COMMAND replay_test)
# points, rectangles and the tessellator from hand-built packets and microcode
add_executable(primitives_test tests/primitives_test.cpp)
target_link_libraries(primitives_test PRIVATE xenoforge_vk stb)
add_test(NAME primitives COMMAND primitives_test)

# the SDK package without a game: installed to a temporary prefix, the example port configured and built against it
# (tests/sdk_test.cmake)
add_test(NAME sdk COMMAND ${CMAKE_COMMAND} -DBUILD=${CMAKE_BINARY_DIR} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}
         -DWORK=${CMAKE_BINARY_DIR}/sdk_test -DCC=${CMAKE_C_COMPILER} -DCXX=${CMAKE_CXX_COMPILER}
         -DGENERATOR=${CMAKE_GENERATOR} -DMAKE=${CMAKE_MAKE_PROGRAM} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/sdk_test.cmake)
set_tests_properties(sdk PROPERTIES TIMEOUT 600)
