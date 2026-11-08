# The SDK package (docs/sdk.md): what `cmake --install build --prefix <dir>`
# puts there for `find_package(XenoForge)` (component "sdk"; the player's flat
# folder is the component "player", package.cmake).
#
#   bin/                      xfrecomp, xfinspect, ffmpeg-xma (the runtime files a port copies beside its executables)
#   include/xenoforge/        the public headers, with the generated code's (game_library.h, ppc_runtime.h, ...)
#   lib/                      the static libraries: core, the kernel (as an archive, linked whole), the renderer
#                             (Vulkan), boot, and with the player: the player, its frontend, Dear ImGui, SDL3
#   lib/cmake/XenoForge/      XenoForgeConfig.cmake and its version file, xenoforge_port.cmake (xenoforge_add_port)
#                             and game_library/ (the project that compiles a port's generated code)
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

set(_sdk_libs xenoforge xenoforge_kernel_archive xenoforge_vk xenoforge_boot)
if(TARGET xenoforge_player)
    list(APPEND _sdk_libs xenoforge_player xf_frontend xf_imgui SDL3-static)
    # SDL3's own link line (the system libraries of its video, input and sound), as this build resolved it
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/sdl3_system_libs.txt
         CONTENT "$<TARGET_PROPERTY:SDL3-static,INTERFACE_LINK_LIBRARIES>\n")
endif()
install(TARGETS ${_sdk_libs} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT sdk)
install(TARGETS xfrecomp xfinspect RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT sdk)
if(XF_XMA)
    install(FILES $<TARGET_FILE:ffmpeg_xma> DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT sdk)
    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE-ffmpeg DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT sdk)
endif()
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/LICENSE-xenia ${CMAKE_SOURCE_DIR}/LICENSE-rexglue
        DESTINATION ${CMAKE_INSTALL_DATADIR}/xenoforge COMPONENT sdk)

# every public header (xenoforge/port.h and the rest as they land), and the interface version the core was built with
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/xenoforge DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT sdk
        FILES_MATCHING PATTERN "*.h" PATTERN "*.inc" PATTERN "*.hpp")
install(FILES ${CMAKE_BINARY_DIR}/generated/xenoforge/game_interface.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/xenoforge COMPONENT sdk)

# the package files
set(XF_SDK_PLAYER ${XF_PLAYER})
if(NOT TARGET xenoforge_player)
    set(XF_SDK_PLAYER OFF)
endif()
set(XF_SDK_XMA ${XF_XMA})
configure_package_config_file(${CMAKE_SOURCE_DIR}/cmake/XenoForgeConfig.cmake.in
                              ${CMAKE_BINARY_DIR}/XenoForgeConfig.cmake INSTALL_DESTINATION ${XF_SDK_CMAKE_DIR})
write_basic_package_version_file(${CMAKE_BINARY_DIR}/XenoForgeConfigVersion.cmake VERSION ${PROJECT_VERSION}
                                 COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_BINARY_DIR}/XenoForgeConfig.cmake ${CMAKE_BINARY_DIR}/XenoForgeConfigVersion.cmake
              ${CMAKE_SOURCE_DIR}/cmake/xenoforge_port.cmake DESTINATION ${XF_SDK_CMAKE_DIR} COMPONENT sdk)
install(FILES ${CMAKE_SOURCE_DIR}/cmake/game_library/CMakeLists.txt DESTINATION ${XF_SDK_CMAKE_DIR}/game_library
        COMPONENT sdk)
if(TARGET xenoforge_player)
    install(FILES ${CMAKE_BINARY_DIR}/sdl3_system_libs.txt DESTINATION ${XF_SDK_CMAKE_DIR} COMPONENT sdk)
endif()
