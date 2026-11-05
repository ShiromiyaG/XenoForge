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
    src/disc.cpp
    src/exports.cpp
    src/xex.cpp
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
add_custom_command(
    OUTPUT ${XF_SHADER_OUT}/rectangle_list.geom.inc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${XF_SHADER_OUT}
    COMMAND ${GLSLC} --target-env=vulkan1.3 -mfmt=num -o ${XF_SHADER_OUT}/rectangle_list.geom.inc
            ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/rectangle_list.geom
    DEPENDS src/shaders/rectangle_list.geom
    COMMENT "glslc rectangle_list.geom")
add_library(xenoforge_vk STATIC src/vk_compute.cpp src/vk_device.cpp src/renderer.cpp
    ${XF_SHADER_OUT}/rectangle_list.geom.inc)
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

# stb_image and stb_image_write (public domain / MIT), for PNGs in the tools;
# Xenia's copy unless STB_DIR says otherwise
if(NOT TARGET stb)
    set(STB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xenia-canary/third_party/stb" CACHE PATH "stb's headers")
    add_library(stb INTERFACE)
    target_include_directories(stb SYSTEM INTERFACE ${STB_DIR})
endif()
add_executable(xfreplay tools/xfreplay.cpp)
target_link_libraries(xfreplay PRIVATE xenoforge_vk stb)
add_executable(xftexture tools/xftexture.cpp)
target_link_libraries(xftexture PRIVATE xenoforge stb)

enable_testing()
foreach(name aes disc xex snappy ucode ucode_parse trace shader_interpreter texture)
    add_executable(${name}_test tests/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE xenoforge)
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()
foreach(name spirv shader_translator)
    add_executable(${name}_test tests/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE xenoforge)
    if(SPIRV_VAL)
        target_compile_definitions(${name}_test PRIVATE XF_SPIRV_VAL="${SPIRV_VAL}")
    endif()
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()
add_executable(vk_compute_test tests/vk_compute_test.cpp)
target_link_libraries(vk_compute_test PRIVATE xenoforge_vk)
add_test(NAME vk_compute COMMAND vk_compute_test)
add_executable(shader_compare_test tests/shader_compare_test.cpp)
target_link_libraries(shader_compare_test PRIVATE xenoforge_vk)
add_test(NAME shader_compare COMMAND shader_compare_test)
