cmake_minimum_required(VERSION 3.25)
# optimized unless asked otherwise
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
project(xenoforge VERSION 0.1.0 LANGUAGES C CXX)

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
    src/exports.cpp
    src/xex.cpp
    src/lzx.cpp
    src/sha1.cpp
    src/sha2.cpp
    src/md5.cpp
    src/snappy.cpp
    src/gpu_trace.cpp
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
    src/guest_space.cpp
    src/recomp_image.cpp
    src/recomp_discover.cpp
    src/recomp_emit.cpp
    src/memory.cpp
    src/guest_memory.cpp
    src/gpu/command_processor.cpp
)
target_include_directories(xenoforge PUBLIC include PRIVATE src)

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
        DEPENDS src/shaders/${source} src/shaders/edram.glsl
        COMMENT "glslc ${name}")
    set(XF_SHADER_WORDS ${XF_SHADER_WORDS} ${XF_SHADER_OUT}/${name}.inc PARENT_SCOPE)
endfunction()
xf_shader(rectangle_list.geom rectangle_list.geom)
xf_shader(point_list.geom point_list.geom)
xf_shader(fullscreen.vert fullscreen.vert)
xf_shader(edram_store.comp edram_store.comp)
xf_shader(edram_store_ms.comp edram_store.comp -DMS)
xf_shader(edram_load.frag edram_load.frag)
xf_shader(edram_load_depth.frag edram_load.frag -DDEPTH)
xf_shader(edram_load_stencil.frag edram_load.frag -DDEPTH -DSTENCIL)
add_library(xenoforge_vk STATIC src/vk_compute.cpp src/vk_device.cpp src/renderer.cpp src/replay.cpp ${XF_SHADER_WORDS})
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

# The kernel (step A4; docs/runtime.md): an object library, so every
# export's registration (XF_EXPORT, a static initializer) is linked in.
file(GLOB XF_KERNEL_SOURCES CONFIGURE_DEPENDS src/kernel/*.cpp)
add_library(xenoforge_kernel OBJECT ${XF_KERNEL_SOURCES})
target_link_libraries(xenoforge_kernel PUBLIC xenoforge)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/memory.cpp) # the guest memory, being written
    add_executable(boot tools/boot.cpp tools/game.cpp)
    target_link_libraries(boot PRIVATE xenoforge_kernel xenoforge)
endif()
# boot linked with a game's generated code: `xfrecomp emit <iso> <XF_GEN_DIR>/<name>`
# writes it (game code: outside the repository), -DXF_GAMES="chaves;ben10" builds
# boot_<name> for each. The code is built here, against these headers, so it
# follows every change to the instructions' semantics.
set(XF_GAMES "" CACHE STRING "games whose generated code a boot_<name> links (directories under XF_GEN_DIR)")
set(XF_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/gen" CACHE PATH "where xfrecomp emit wrote each game")
foreach(game ${XF_GAMES})
    file(GLOB game_sources CONFIGURE_DEPENDS ${XF_GEN_DIR}/${game}/functions_*.cpp ${XF_GEN_DIR}/${game}/table.cpp)
    if(NOT game_sources)
        message(FATAL_ERROR "XF_GAMES: no generated code in ${XF_GEN_DIR}/${game}")
    endif()
    add_library(game_${game} STATIC ${game_sources})
    target_include_directories(game_${game} PRIVATE include ${XF_GEN_DIR}/${game})
    # optimized, without debug information (it is large) or warnings; never
    # -ffast-math (docs/recompiler.md)
    target_compile_options(game_${game} PRIVATE -O2 -g0 -w -mfma -mavx2)
    add_executable(boot_${game} tools/boot.cpp tools/game.cpp)
    target_compile_definitions(boot_${game} PRIVATE XF_GENERATED)
    target_link_libraries(boot_${game} PRIVATE xenoforge_kernel xenoforge game_${game})
endforeach()
# the kernel's threads and objects, tested through their exports and
# src/kernel/objects.h
add_executable(kernel_threads_test tests/kernel_threads_test.cpp)
target_include_directories(kernel_threads_test PRIVATE src/kernel)
target_link_libraries(kernel_threads_test PRIVATE xenoforge_kernel xenoforge)

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
if(TARGET boot)
    # boot --render: the renderer as the command processor's Sink, and its shots
    target_link_libraries(boot PRIVATE xenoforge_vk stb)
endif()
foreach(game ${XF_GAMES})
    target_link_libraries(boot_${game} PRIVATE xenoforge_vk stb)
endforeach()
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
    FetchContent_MakeAvailable(SDL3 tomlplusplus imgui)
    target_compile_options(SDL3-static PRIVATE -w) # the project's -Wall is not for SDL's sources
    add_library(xf_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
    target_include_directories(xf_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(xf_imgui PUBLIC SDL3::SDL3-static Vulkan::Vulkan)
    target_compile_options(xf_imgui PRIVATE -w)
    # the window, input, sound and settings, apart from the kernel they feed
    add_library(xf_frontend STATIC frontend/settings.cpp frontend/presenter.cpp frontend/sdl_input.cpp
                                   frontend/sdl_audio.cpp)
    target_link_libraries(xf_frontend PUBLIC xenoforge SDL3::SDL3-static Vulkan::Vulkan tomlplusplus::tomlplusplus)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(xf_frontend PRIVATE -Wno-missing-field-initializers)
    endif()
    set(XF_PLAYERS player)
    add_executable(player frontend/player.cpp tools/game.cpp)
    foreach(game ${XF_GAMES})
        add_executable(player_${game} frontend/player.cpp tools/game.cpp)
        target_compile_definitions(player_${game} PRIVATE XF_GENERATED)
        target_link_libraries(player_${game} PRIVATE game_${game})
        list(APPEND XF_PLAYERS player_${game})
    endforeach()
    foreach(target ${XF_PLAYERS})
        target_link_libraries(${target} PRIVATE xf_frontend xf_imgui xenoforge_kernel xenoforge_vk stb)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${target} PRIVATE -Wno-missing-field-initializers)
        endif()
    endforeach()
    # the settings file, the sound's fold to stereo, the scripted presses
    add_executable(frontend_test tests/frontend_test.cpp tools/game.cpp)
    target_link_libraries(frontend_test PRIVATE xf_frontend xenoforge_kernel xenoforge_vk)
    add_test(NAME frontend COMMAND frontend_test)
endif()
add_executable(xftexture tools/xftexture.cpp)
target_link_libraries(xftexture PRIVATE xenoforge stb)

enable_testing()
foreach(name aes disc xex lzx snappy ucode ucode_parse trace shader_interpreter texture ppc memory command_processor
             recomp_discover)
    add_executable(${name}_test tests/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE xenoforge)
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()
# the kernel's exports, called through a Kernel (the object library brings
# their registrations)
foreach(name kernel_memory kernel_modules kernel_crypt)
    add_executable(${name}_test tests/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE xenoforge_kernel xenoforge)
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()
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
add_test(NAME kernel_threads COMMAND kernel_threads_test)
# a rare deadlock seen once under load (2026-11-05, not reproduced in 36
# runs since): fail instead of hanging the suite
set_tests_properties(kernel_threads PROPERTIES TIMEOUT 120)
# the video family: the GPU live through its exports, MMIO and threads
add_executable(kernel_video_test tests/kernel_video_test.cpp)
target_include_directories(kernel_video_test PRIVATE src/kernel)
target_link_libraries(kernel_video_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_video COMMAND kernel_video_test)
set_tests_properties(kernel_video PROPERTIES TIMEOUT 120) # threads: fail, never hang
# the files family and XAM, through their exports on a guest thread
add_executable(kernel_files_test tests/kernel_files_test.cpp)
target_include_directories(kernel_files_test PRIVATE src/kernel)
target_link_libraries(kernel_files_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_files COMMAND kernel_files_test)
set_tests_properties(kernel_files PROPERTIES TIMEOUT 120) # threads: fail, never hang
# the sound family: render driver clients and XMA contexts on guest threads
add_executable(kernel_audio_test tests/kernel_audio_test.cpp)
target_include_directories(kernel_audio_test PRIVATE src/kernel)
target_link_libraries(kernel_audio_test PRIVATE xenoforge_kernel xenoforge)
add_test(NAME kernel_audio COMMAND kernel_audio_test)
set_tests_properties(kernel_audio PROPERTIES TIMEOUT 120) # threads: fail, never hang
add_executable(vk_compute_test tests/vk_compute_test.cpp)
target_link_libraries(vk_compute_test PRIVATE xenoforge_vk)
add_test(NAME vk_compute COMMAND vk_compute_test)
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
add_executable(shader_compare_test tests/shader_compare_test.cpp)
target_link_libraries(shader_compare_test PRIVATE xenoforge_vk)
add_test(NAME shader_compare COMMAND shader_compare_test)
# the renderer against Xenia's pictures, where XF_TRACES and XF_DUMPS say
add_executable(replay_test tests/replay_test.cpp)
target_link_libraries(replay_test PRIVATE xenoforge_vk stb)
add_test(NAME replay COMMAND replay_test)
