# The SDK package, end to end without a game (docs/sdk.md): installs the SDK
# component to a temporary prefix, configures sdk/examples/minimal_port against it
# (find_package(XenoForge), xenoforge_add_port on tests/sdk's empty generated
# code), builds it and runs what it built; then sdk/examples/native_port the same
# way (its shaders through glslc), when the Vulkan SDK's glslc is there; then
# a port made by the installed new_port.cmake from the template. The recompile of a real disc is
# the manual check (docs/sdk.md); this one is the package: headers, libraries,
# the imported targets, the function, the runtime files beside the executables.
#
#   cmake -DBUILD=<build dir> -DSOURCE=<source dir> -DWORK=<scratch dir> -DCC=<c> -DCXX=<c++> -DGENERATOR=Ninja
#         -DMAKE=<ninja> -P sdk_test.cmake

foreach(v BUILD SOURCE WORK CC CXX GENERATOR MAKE)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "sdk_test: -D${v} is missing")
    endif()
endforeach()

function(step what)
    message(STATUS "sdk_test: ${what}")
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "sdk_test: ${what} failed (${rc}):\n${out}")
    endif()
    set(step_output "${out}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})
set(prefix ${WORK}/prefix)
step("install" ${CMAKE_COMMAND} --install ${BUILD} --component sdk --prefix ${prefix})
foreach(f include/xenoforge/run.h include/xenoforge/native_render.h include/xenoforge/game_library.h include/xenoforge/ppc_runtime.h
          lib/cmake/XenoForge/XenoForgeConfig.cmake lib/cmake/XenoForge/XenoForgeConfigVersion.cmake
          lib/cmake/XenoForge/xenoforge_port.cmake)
    if(NOT EXISTS ${prefix}/${f})
        message(FATAL_ERROR "sdk_test: ${f} is not in the package")
    endif()
endforeach()
# only the port's API and what it includes: XenoForge's own headers stay out
foreach(f include/xenoforge/renderer.h include/xenoforge/recomp.h include/xenoforge/stencil.h)
    if(EXISTS ${prefix}/${f})
        message(FATAL_ERROR "sdk_test: ${f} is in the package (only the port's API and what it includes go)")
    endif()
endforeach()
file(GLOB tools ${prefix}/bin/xfrecomp* ${prefix}/bin/xfinspect*)
list(LENGTH tools n)
if(NOT n EQUAL 2)
    message(FATAL_ERROR "sdk_test: xfrecomp and xfinspect are not both in bin/ (${tools})")
endif()

step("configure the example" ${CMAKE_COMMAND} -S ${SOURCE}/sdk/examples/minimal_port -B ${WORK}/example -G ${GENERATOR}
     -DCMAKE_MAKE_PROGRAM=${MAKE} -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_COMPILER=${CXX} -DCMAKE_PREFIX_PATH=${prefix}
     -DMINIMAL_GENERATED_DIR=${SOURCE}/tests/sdk)
step("build the example" ${CMAKE_COMMAND} --build ${WORK}/example)

# what it built answers like boot: the usage with no arguments, a bad option's message
file(GLOB booted ${WORK}/example/boot_minimal*)
list(FILTER booted EXCLUDE REGEX "\\.(pdb|ilk|exp|lib)$")
list(GET booted 0 boot_exe)
execute_process(COMMAND ${boot_exe} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 2 OR NOT out MATCHES "usage: boot")
    message(FATAL_ERROR "sdk_test: boot_minimal without arguments: ${rc}\n${out}")
endif()
execute_process(COMMAND ${boot_exe} nothing.iso --option nothing=1 RESULT_VARIABLE rc OUTPUT_VARIABLE out
                ERROR_VARIABLE out)
if(NOT rc EQUAL 2 OR NOT out MATCHES "no option nothing")
    message(FATAL_ERROR "sdk_test: boot_minimal with a bad option: ${rc}\n${out}")
endif()
# the native rendering example: the header, the renderer's library and a port's own SPIR-V
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin)
if(GLSLC)
    step("configure the native example" ${CMAKE_COMMAND} -S ${SOURCE}/sdk/examples/native_port -B ${WORK}/native -G
         ${GENERATOR} -DCMAKE_MAKE_PROGRAM=${MAKE} -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_COMPILER=${CXX}
         -DCMAKE_PREFIX_PATH=${prefix} -DNATIVE_GENERATED_DIR=${SOURCE}/tests/sdk -DGLSLC=${GLSLC})
    step("build the native example" ${CMAKE_COMMAND} --build ${WORK}/native)
    file(GLOB booted ${WORK}/native/boot_native*)
    list(FILTER booted EXCLUDE REGEX "\\.(pdb|ilk|exp|lib)$")
    list(GET booted 0 boot_exe)
    execute_process(COMMAND ${boot_exe} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT rc EQUAL 2 OR NOT out MATCHES "usage: boot")
        message(FATAL_ERROR "sdk_test: boot_native without arguments: ${rc}\n${out}")
    endif()
else()
    message(STATUS "sdk_test: no glslc, the native example left out")
endif()
# a new port's repository from the installed template, built on the empty table as the example is
step("make a port from the template" ${CMAKE_COMMAND} -DNAME=template_port "-DTITLE=Template port"
     -DDIR=${WORK}/template_port -P ${prefix}/share/xenoforge/new_port.cmake)
foreach(f CMakeLists.txt CMakePresets.json CMakeUserPresets.json project.toml src/main.cpp src/hooks.cpp .gitignore)
    if(NOT EXISTS ${WORK}/template_port/${f})
        message(FATAL_ERROR "sdk_test: the template wrote no ${f}")
    endif()
endforeach()
step("configure the template's port" ${CMAKE_COMMAND} -S ${WORK}/template_port -B ${WORK}/template_port/build -G
     ${GENERATOR} -DCMAKE_MAKE_PROGRAM=${MAKE} -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_COMPILER=${CXX}
     -DXENOFORGE_SDK=${prefix} -DGAME_GENERATED_DIR=${SOURCE}/tests/sdk)
step("build the template's port" ${CMAKE_COMMAND} --build ${WORK}/template_port/build)
file(GLOB booted ${WORK}/template_port/build/boot_template_port*)
list(FILTER booted EXCLUDE REGEX "\\.(pdb|ilk|exp|lib)$")
list(GET booted 0 boot_exe)
execute_process(COMMAND ${boot_exe} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 2 OR NOT out MATCHES "usage: boot")
    message(FATAL_ERROR "sdk_test: boot_template_port without arguments: ${rc}\n${out}")
endif()
file(GLOB hooks_module ${WORK}/template_port/build/template_port_hooks*)
if(NOT hooks_module)
    message(FATAL_ERROR "sdk_test: the template's port built no hooks module")
endif()

# the same port from an executable, as from a disc (xenoforge_add_port's ISO form: build_game.cmake's xfrecomp emit,
# the generated code configured and compiled), then run: the boot report has the code linked in
if(MAKE_XEX)
    step("make an executable" ${MAKE_XEX} ${WORK}/default.xex)
    step("configure the template's port from the executable" ${CMAKE_COMMAND} -S ${WORK}/template_port -B
         ${WORK}/template_port/build_iso -G ${GENERATOR} -DCMAKE_MAKE_PROGRAM=${MAKE} -DCMAKE_C_COMPILER=${CC}
         -DCMAKE_CXX_COMPILER=${CXX} -DXENOFORGE_SDK=${prefix} -DGAME_ISO=${WORK}/default.xex)
    step("build the template's port from the executable" ${CMAKE_COMMAND} --build ${WORK}/template_port/build_iso)
    if(NOT step_output MATCHES "xfrecomp emit" OR NOT step_output MATCHES "compilers at once")
        message(FATAL_ERROR "sdk_test: the build from the executable did not emit and compile:\n${step_output}")
    endif()
    file(GLOB booted ${WORK}/template_port/build_iso/boot_template_port*)
    list(FILTER booted EXCLUDE REGEX "\\.(pdb|ilk|exp|lib)$")
    list(GET booted 0 boot_exe)
    execute_process(COMMAND ${boot_exe} ${WORK}/default.xex --seconds 2 RESULT_VARIABLE rc OUTPUT_VARIABLE out
                    ERROR_VARIABLE out TIMEOUT 120)
    if(NOT out MATCHES "generated code: 1 modules, 4 functions" OR NOT out MATCHES "cpu: linked generated code")
        message(FATAL_ERROR "sdk_test: boot_template_port on the executable: ${rc}\n${out}")
    endif()
    # the project file changed: emitted again, the generated code's project configured a second time (its cache
    # once turned it to the debug runtime there, which the link refused), built and linked again
    file(APPEND ${WORK}/template_port/project.toml "\"0x82001000\" = \"Entry\"\n")
    step("build the template's port again after a project file change" ${CMAKE_COMMAND} --build
         ${WORK}/template_port/build_iso)
    if(NOT step_output MATCHES "1 names")
        message(FATAL_ERROR "sdk_test: the project file's change was not emitted:\n${step_output}")
    endif()
endif()
message(STATUS "sdk_test: ok")
