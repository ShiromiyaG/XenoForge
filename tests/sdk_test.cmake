# The SDK package, end to end without a game (docs/sdk.md): installs the SDK
# component to a temporary prefix, configures examples/minimal_port against it
# (find_package(XenoForge), xenoforge_add_port on tests/sdk's empty generated
# code), builds it and runs what it built. The recompile of a real disc is
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
foreach(f include/xenoforge/run.h include/xenoforge/game_library.h include/xenoforge/ppc_runtime.h
          lib/cmake/XenoForge/XenoForgeConfig.cmake lib/cmake/XenoForge/XenoForgeConfigVersion.cmake
          lib/cmake/XenoForge/xenoforge_port.cmake)
    if(NOT EXISTS ${prefix}/${f})
        message(FATAL_ERROR "sdk_test: ${f} is not in the package")
    endif()
endforeach()
file(GLOB tools ${prefix}/bin/xfrecomp* ${prefix}/bin/xfinspect*)
list(LENGTH tools n)
if(NOT n EQUAL 2)
    message(FATAL_ERROR "sdk_test: xfrecomp and xfinspect are not both in bin/ (${tools})")
endif()

step("configure the example" ${CMAKE_COMMAND} -S ${SOURCE}/examples/minimal_port -B ${WORK}/example -G ${GENERATOR}
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
message(STATUS "sdk_test: ok")
