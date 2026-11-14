# A port's generated code from its disc, the build step of xenoforge_add_port's
# ISO form (run with cmake -P; installed beside xenoforge_port.cmake):
#
#   1. xfrecomp emit <ISO> <GEN>/code [--xex XEX] [--project PROJECT]
#      (writes only the files whose text changed)
#   2. the project in GAME_LIBRARY_DIR configured over what was written (its
#      glob sees every file), for the port's compiler and target
#   3. built with JOBS compilers at once, or, when JOBS is 0, as many as
#      `xfrecomp jobs` says: the recompile step's policy, the cores and the
#      memory free now (each compiler of a functions file needs a good part
#      of a gigabyte, so a machine's cores alone would run it out of memory)
#
# Parameters (-D): XFRECOMP, ISO, GEN, XEX, PROJECT, GAME_LIBRARY_DIR, NAME,
# GENERATOR, MAKE_PROGRAM, C_COMPILER, CXX_COMPILER, INCLUDE_DIR, OPT and
# CPU_FLAGS (space-separated), JOBS, and the target's TOOLCHAIN_FILE, SYSROOT,
# ANDROID_ABI, ANDROID_PLATFORM, ANDROID_STL when set.

foreach(v XFRECOMP ISO GEN GAME_LIBRARY_DIR NAME GENERATOR CXX_COMPILER INCLUDE_DIR)
    if(NOT DEFINED ${v} OR "${${v}}" STREQUAL "")
        message(FATAL_ERROR "build_game: -D${v} is missing")
    endif()
endforeach()

function(run what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${NAME}: ${what} failed (${rc})")
    endif()
endfunction()

# 1. the emission
set(emit ${XFRECOMP} emit ${ISO} ${GEN}/code)
if(XEX)
    list(APPEND emit --xex ${XEX})
endif()
if(PROJECT)
    list(APPEND emit --project ${PROJECT})
endif()
run("xfrecomp emit" ${emit})

# 2. the project over it
set(configure ${CMAKE_COMMAND} -S ${GAME_LIBRARY_DIR} -B ${GEN}/build -G ${GENERATOR} -DXF_GAME_NAME=${NAME}
    -DXF_GAME_CODE=${GEN}/code -DXF_GAME_OUT=${GEN}/lib -DXF_INCLUDE_DIR=${INCLUDE_DIR} "-DXF_GAME_OPT=${OPT}"
    "-DXF_GAME_CPU_FLAGS=${CPU_FLAGS}" -DCMAKE_CXX_COMPILER=${CXX_COMPILER})
foreach(v C_COMPILER MAKE_PROGRAM)
    if(${v})
        list(APPEND configure -DCMAKE_${v}=${${v}})
    endif()
endforeach()
foreach(v TOOLCHAIN_FILE SYSROOT)
    if(${v})
        list(APPEND configure -DCMAKE_${v}=${${v}})
    endif()
endforeach()
foreach(v ANDROID_ABI ANDROID_PLATFORM ANDROID_STL)
    if(${v})
        list(APPEND configure -D${v}=${${v}})
    endif()
endforeach()
run("configuring the generated code" ${configure})

# 3. the compile, as many compilers at once as the memory allows
if(NOT JOBS)
    execute_process(COMMAND ${XFRECOMP} jobs OUTPUT_VARIABLE JOBS ERROR_VARIABLE why RESULT_VARIABLE rc
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
    if(NOT rc EQUAL 0 OR NOT JOBS MATCHES "^[0-9]+$")
        set(JOBS 2)
        set(why "xfrecomp jobs did not answer: 2")
    endif()
    message(STATUS "${NAME}: the generated code, ${why}")
else()
    message(STATUS "${NAME}: the generated code, ${JOBS} compilers at once (XF_GAME_JOBS)")
endif()
run("compiling the generated code" ${CMAKE_COMMAND} --build ${GEN}/build --parallel ${JOBS})
