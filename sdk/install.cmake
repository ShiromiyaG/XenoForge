# The SDK package (docs/sdk.md): what `cmake --install build --prefix <dir>`
# puts there for `find_package(XenoForge)` (component "sdk"; the player's flat
# folder is the component "player", package.cmake).
#
#   bin/                      xfrecomp, xfinspect, ffmpeg-xma (the runtime files a port copies beside its executables)
#   include/xenoforge/        the port's API (run.h, port.h, native_render.h) and the generated code's (game_library.h,
#                             ppc_runtime.h, ...), with what they include: not XenoForge's other headers
#   lib/                      the static libraries: core, the kernel (as an archive, linked whole), the renderer
#                             (Vulkan, with volk), boot, and with the player: the player, its frontend, Dear ImGui, SDL3
#   lib/cmake/XenoForge/      XenoForgeConfig.cmake and its version file, xenoforge_port.cmake (xenoforge_add_port)
#                             and game_library/ (the project that compiles a port's generated code)
#   share/xenoforge/          the licenses, new_port.cmake and template/ (a new port's repository), toolchains/
#
# The imported targets (XenoForge::core, ::kernel, ::vk, ::boot, ::player and
# the executable ::xfrecomp) are written by hand in XenoForgeConfig.cmake.in:
# the libraries link third-party static libraries built in this tree that no
# export set holds, so CMake's own export would refuse them.
#
# Included by CMakeLists.txt after every target exists.

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(XF_SDK_CMAKE_DIR lib/cmake/XenoForge)

# the kernel's objects as an archive: they register the exports with static
# initializers, so a port links the archive whole (the object library is not
# installable as a library)
add_library(xenoforge_kernel_archive STATIC $<TARGET_OBJECTS:xenoforge_kernel>)
set_target_properties(xenoforge_kernel_archive PROPERTIES OUTPUT_NAME xenoforge_kernel LINKER_LANGUAGE CXX)

set(_sdk_libs xenoforge xenoforge_kernel_archive xenoforge_vk xf_volk xenoforge_boot)
if(TARGET xenoforge_player)
    list(APPEND _sdk_libs xenoforge_player xf_frontend xf_imgui SDL3-static)
    # SDL3's own link line (the system libraries of its video, input and sound), as this build resolved it
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/sdl3_system_libs.txt
         CONTENT "$<TARGET_PROPERTY:SDL3-static,INTERFACE_LINK_LIBRARIES>\n")
endif()
install(TARGETS ${_sdk_libs} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT sdk)
install(TARGETS xfrecomp xfinspect xfshader RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT sdk)
if(XF_XMA)
    install(FILES $<TARGET_FILE:ffmpeg_xma> DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT sdk)
    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE-ffmpeg DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT sdk)
endif()
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/LICENSE-xenia ${CMAKE_SOURCE_DIR}/LICENSE-rexglue
        DESTINATION ${CMAKE_INSTALL_DATADIR}/xenoforge COMPONENT sdk)
# a new port's repository from the template: cmake -DNAME=<name> -P <prefix>/share/xenoforge/new_port.cmake
install(FILES ${CMAKE_SOURCE_DIR}/sdk/new_port.cmake DESTINATION ${CMAKE_INSTALL_DATADIR}/xenoforge COMPONENT sdk)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/sdk/template DESTINATION ${CMAKE_INSTALL_DATADIR}/xenoforge COMPONENT sdk)
# the toolchain files a port cross-compiles with (ARM64 Linux; Android's is the NDK's)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/cmake/toolchains DESTINATION ${CMAKE_INSTALL_DATADIR}/xenoforge COMPONENT sdk)

# The headers a port sees: its API (what a port's code includes, and what its
# generated code includes) and every header they include, followed here
# through their #include "xenoforge/..." lines, so the list keeps up with the
# headers. The rest of include/xenoforge (the renderer's, the recompiler's,
# the disc's, the tools') is XenoForge's own and is not installed.
set(XF_SDK_API_HEADERS
    run.h port.h native_render.h native_shaders.h port_module.h cpu_level_check.h # a port's code (cpu_level_check.h:
                                                                                  # xenoforge_add_port's)
    ppc_generated.h ppc_module.h game_library.h ppc_runtime.h)   # its generated code
set(_xf_todo ${XF_SDK_API_HEADERS})
set(XF_SDK_HEADERS)
while(_xf_todo)
    list(POP_FRONT _xf_todo _xf_header)
    if(_xf_header IN_LIST XF_SDK_HEADERS)
        continue()
    endif()
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/include/xenoforge/${_xf_header})
        message(FATAL_ERROR "the SDK's headers: no include/xenoforge/${_xf_header}")
    endif()
    list(APPEND XF_SDK_HEADERS ${_xf_header})
    file(STRINGS ${CMAKE_SOURCE_DIR}/include/xenoforge/${_xf_header} _xf_lines REGEX "^#include \"xenoforge/")
    foreach(_xf_line ${_xf_lines})
        string(REGEX REPLACE "^[^\"]*\"xenoforge/([^\"]+)\".*$" "\\1" _xf_included "${_xf_line}")
        list(APPEND _xf_todo ${_xf_included})
    endforeach()
endwhile()
list(SORT XF_SDK_HEADERS)
# re-read when a header changes (an #include added or dropped)
list(TRANSFORM XF_SDK_HEADERS PREPEND ${CMAKE_SOURCE_DIR}/include/xenoforge/ OUTPUT_VARIABLE _xf_paths)
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_xf_paths})
foreach(_xf_header ${XF_SDK_HEADERS})
    get_filename_component(_xf_dir ${_xf_header} DIRECTORY)
    install(FILES ${CMAKE_SOURCE_DIR}/include/xenoforge/${_xf_header}
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/xenoforge/${_xf_dir} COMPONENT sdk)
endforeach()
list(LENGTH XF_SDK_HEADERS _xf_count)
message(STATUS "the SDK's headers: ${_xf_count} (${XF_SDK_API_HEADERS} and what they include)")
# Khronos's headers and volk's (cmake/vulkan.cmake), so a port needs no Vulkan SDK
install(DIRECTORY ${XF_VULKAN_INCLUDE}/vulkan ${XF_VULKAN_INCLUDE}/vk_video DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT sdk)
install(FILES ${XF_VOLK_INCLUDE}/volk.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT sdk)
install(FILES ${XF_SPIRV_INCLUDE}/spirv.hpp11 ${XF_SPIRV_INCLUDE}/GLSL.std.450.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/spirv-headers COMPONENT sdk)
install(FILES ${CMAKE_BINARY_DIR}/generated/xenoforge/game_interface.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/xenoforge COMPONENT sdk)

# the package files
set(XF_SDK_PLAYER ${XF_PLAYER})
if(NOT TARGET xenoforge_player)
    set(XF_SDK_PLAYER OFF)
endif()
set(XF_SDK_XMA ${XF_XMA})
configure_package_config_file(${CMAKE_SOURCE_DIR}/sdk/XenoForgeConfig.cmake.in
                              ${CMAKE_BINARY_DIR}/XenoForgeConfig.cmake INSTALL_DESTINATION ${XF_SDK_CMAKE_DIR})
write_basic_package_version_file(${CMAKE_BINARY_DIR}/XenoForgeConfigVersion.cmake VERSION ${PROJECT_VERSION}
                                 COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_BINARY_DIR}/XenoForgeConfig.cmake ${CMAKE_BINARY_DIR}/XenoForgeConfigVersion.cmake
              ${CMAKE_SOURCE_DIR}/sdk/xenoforge_port.cmake ${CMAKE_SOURCE_DIR}/sdk/build_game.cmake
        DESTINATION ${XF_SDK_CMAKE_DIR} COMPONENT sdk)
install(FILES ${CMAKE_SOURCE_DIR}/sdk/game_library/CMakeLists.txt DESTINATION ${XF_SDK_CMAKE_DIR}/game_library
        COMPONENT sdk)
if(TARGET xenoforge_player)
    install(FILES ${CMAKE_BINARY_DIR}/sdl3_system_libs.txt DESTINATION ${XF_SDK_CMAKE_DIR} COMPONENT sdk)
endif()
