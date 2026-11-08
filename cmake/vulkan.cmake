# Vulkan without the Vulkan SDK (Xenia's model): Khronos's headers and volk
# fetched at configure time, the Vulkan loader found at run time, and the
# internal shaders committed as SPIR-V.
#
#   xf_vulkan_headers  Vulkan-Headers' include directory (Apache-2.0/MIT), no prototypes
#   xf_volk            volk (MIT): the Vulkan functions as pointers volkInitialize
#                      fills from the system's loader (vulkan-1.dll, libvulkan.so.1),
#                      so nothing links an import library
#   SPIRV-Headers      spirv.hpp11 and GLSL.std.450.h (MIT), for the translator
#
# Each FetchContent source can be given instead with
# -DFETCHCONTENT_SOURCE_DIR_<NAME>=<dir> (VULKANHEADERS, VOLK, SPIRVHEADERS),
# for a build without the network.
#
# The internal shaders' SPIR-V lives in src/shaders/spirv/<name>.inc (lists of
# words, glslc -mfmt=num), committed, so a build needs no shader compiler.
# Where glslc is found (the Vulkan SDK's, or GLSLC=<path>) and
# XF_SHADERS_REGENERATE is on (the default then), the build compiles each
# shader again into that directory whenever its source changes: commit the
# .inc with the .glsl/.comp/.frag that made it.

include(FetchContent)
set(XF_VULKAN_SDK_TAG 1.4.341)
FetchContent_Declare(VulkanHeaders GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
                     GIT_TAG v${XF_VULKAN_SDK_TAG} GIT_SHALLOW TRUE SOURCE_SUBDIR no-cmake)
FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git
                     GIT_TAG vulkan-sdk-${XF_VULKAN_SDK_TAG}.0 GIT_SHALLOW TRUE SOURCE_SUBDIR no-cmake)
FetchContent_Declare(SPIRVHeaders GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
                     GIT_TAG vulkan-sdk-${XF_VULKAN_SDK_TAG}.0 GIT_SHALLOW TRUE SOURCE_SUBDIR no-cmake)
# SOURCE_SUBDIR names no directory: the sources are only fetched, their own CMake projects never added
FetchContent_MakeAvailable(VulkanHeaders volk SPIRVHeaders)
set(XF_VULKAN_INCLUDE ${vulkanheaders_SOURCE_DIR}/include)
set(XF_VOLK_INCLUDE ${volk_SOURCE_DIR})
set(XF_SPIRV_INCLUDE ${spirvheaders_SOURCE_DIR}/include/spirv/unified1)

add_library(xf_vulkan_headers INTERFACE)
target_include_directories(xf_vulkan_headers SYSTEM INTERFACE ${XF_VULKAN_INCLUDE})

add_library(xf_volk STATIC ${volk_SOURCE_DIR}/volk.c)
target_include_directories(xf_volk SYSTEM PUBLIC ${XF_VOLK_INCLUDE} ${XF_VULKAN_INCLUDE})
# every user sees Vulkan without prototypes: a call made through vulkan.h
# alone is a compile error, never an unresolved symbol
target_compile_definitions(xf_volk PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(xf_volk PUBLIC ${CMAKE_DL_LIBS})
set_target_properties(xf_volk PROPERTIES OUTPUT_NAME xenoforge_volk POSITION_INDEPENDENT_CODE ON)
target_compile_options(xf_volk PRIVATE $<$<C_COMPILER_ID:Clang,GNU>:-w>)

# the shaders
set(XF_SPIRV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/spirv)
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(GLSLC)
    set(_xf_regen_default ON)
else()
    set(_xf_regen_default OFF)
endif()
option(XF_SHADERS_REGENERATE "compile the internal shaders again into src/shaders/spirv when they change (needs glslc)"
       ${_xf_regen_default})
if(XF_SHADERS_REGENERATE AND NOT GLSLC)
    message(FATAL_ERROR "XF_SHADERS_REGENERATE needs glslc (the Vulkan SDK's, or -DGLSLC=<path>)")
endif()

# xf_spirv(<list var> <name> <source> [DEPENDS files...] [glslc arguments...]): the
# committed src/shaders/spirv/<name>.inc, appended to <list var>; compiled again
# from <source> (relative to the source directory) when regenerating
function(xf_spirv list name source)
    cmake_parse_arguments(PARSE_ARGV 3 arg "" "" "DEPENDS")
    set(out ${XF_SPIRV_DIR}/${name}.inc)
    if(XF_SHADERS_REGENERATE)
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/spirv
            COMMAND ${GLSLC} --target-env=vulkan1.3 -mfmt=num ${arg_UNPARSED_ARGUMENTS}
                    -MD -MF ${CMAKE_CURRENT_BINARY_DIR}/spirv/${name}.d -MT ${out}
                    -o ${out} ${CMAKE_CURRENT_SOURCE_DIR}/${source}
            DEPENDS ${source} ${arg_DEPENDS}
            DEPFILE ${CMAKE_CURRENT_BINARY_DIR}/spirv/${name}.d
            COMMENT "glslc ${name}")
    elseif(NOT EXISTS ${out})
        message(FATAL_ERROR "${out} is missing: build once with glslc (XF_SHADERS_REGENERATE) and commit it")
    endif()
    set(${list} ${${list}} ${out} PARENT_SCOPE)
endfunction()
