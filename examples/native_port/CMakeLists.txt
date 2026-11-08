# A port that draws its own frames (docs/sdk.md, "Native rendering"), built on
# an installed XenoForge as examples/minimal_port is:
#
#   cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<XenoForge's prefix> -DNATIVE_ISO=<disc image>
#   cmake --build build
#   build/player_native <disc image> [--capture <hex address>]
#
# Its shaders are GLSL compiled to SPIR-V by glslc (the Vulkan SDK's) at the
# build, as a port's own shaders are.
cmake_minimum_required(VERSION 3.25)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
project(native_port LANGUAGES CXX)

# inside XenoForge's own build (-DXF_NATIVE_PORT_GAME) the targets are there already
if(NOT TARGET XenoForge::vk)
    find_package(XenoForge REQUIRED)
endif()

find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin)
if(NOT GLSLC)
    message(FATAL_ERROR "native_port: glslc not found (the Vulkan SDK's; set VULKAN_SDK or GLSLC)")
endif()

# each shader as a list of SPIR-V words main.cpp includes
set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${shader_dir})
set(shader_words)
foreach(shader triangle.vert triangle.frag background.vert background.frag)
    set(out ${shader_dir}/${shader}.inc)
    add_custom_command(OUTPUT ${out}
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=num -o ${out} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}
        COMMENT "glslc ${shader}")
    list(APPEND shader_words ${out})
endforeach()
add_custom_target(native_port_shaders DEPENDS ${shader_words})

set(NATIVE_ISO "" CACHE FILEPATH "the game's disc image")
set(NATIVE_GENERATED_DIR "" CACHE PATH "a directory xfrecomp emit wrote")
set(NATIVE_PREBUILT "" CACHE FILEPATH "a game library built before (game_<name>.lib)")
set(NATIVE_PROJECT "" CACHE FILEPATH "the port's project file")

set(how)
if(NATIVE_PREBUILT)
    set(how PREBUILT ${NATIVE_PREBUILT})
elseif(NATIVE_GENERATED_DIR)
    set(how GENERATED_DIR ${NATIVE_GENERATED_DIR})
elseif(NATIVE_ISO)
    set(how ISO ${NATIVE_ISO})
    if(NATIVE_PROJECT)
        list(APPEND how PROJECT ${NATIVE_PROJECT})
    endif()
else()
    message(FATAL_ERROR "set NATIVE_ISO to the game's disc image")
endif()
xenoforge_add_port(native ${how} SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp INCLUDES ${shader_dir})
foreach(kind player boot)
    if(TARGET ${kind}_native)
        add_dependencies(${kind}_native native_port_shaders)
        # Vulkan's structures are filled field by field after their type
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${kind}_native PRIVATE -Wno-missing-field-initializers)
        endif()
    endif()
endforeach()
