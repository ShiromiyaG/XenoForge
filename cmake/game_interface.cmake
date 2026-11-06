# The game library's interface (include/xenoforge/game_library.h), made at
# build time: cmake -DROOT=<source dir> -DOUT=<dir> -P game_interface.cmake
#
# - OUT/xenoforge/game_interface.h: XF_GAME_INTERFACE_VERSION, the first 32
#   bits of a SHA-256 of what makes a game library: the headers the generated
#   code compiles against, the recompiler's sources (discovery and emission)
#   and src/game_build.cpp (the compiler's flags). A change to any of them
#   makes every cached library stale, so the launcher builds it again.
# - OUT/game_headers.inc: those headers' text, which the recompile step writes
#   beside the code it compiles (game_build.cpp), so the code is always built
#   against the headers of the XenoForge that loads it.

set(headers
    game_library.h
    ppc.h
    ppc_context.h
    ppc_generated.h
    ppc_module.h
    ppc_opcodes.inc
    ppc_ops.h
    ppc_runtime.h
    ppc_ops/control.h
    ppc_ops/fpu.h
    ppc_ops/integer.h
    ppc_ops/memory.h
    ppc_ops/vmx.h
    ppc_ops/vmx_float.h)
file(GLOB recipe RELATIVE ${ROOT} ${ROOT}/src/recomp_*.cpp)
list(SORT recipe)
list(APPEND recipe include/xenoforge/recomp.h src/game_build.cpp)

set(digest_input "")
set(inc "// Written by cmake/game_interface.cmake: the headers a game library compiles against.\n")
foreach(h ${headers})
    file(READ ${ROOT}/include/xenoforge/${h} text)
    string(REPLACE "\r\n" "\n" text "${text}")
    string(APPEND digest_input "xenoforge/${h}\n${text}\n")
    string(APPEND inc "{\"xenoforge/${h}\", R\"xf_header(${text})xf_header\"},\n")
endforeach()
foreach(f ${recipe})
    file(READ ${ROOT}/${f} text)
    string(REPLACE "\r\n" "\n" text "${text}")
    string(APPEND digest_input "${f}\n${text}\n")
endforeach()
string(SHA256 digest "${digest_input}")
string(SUBSTRING ${digest} 0 8 version)

set(header "// Written by cmake/game_interface.cmake: the game library's interface version (game_library.h).\n")
string(APPEND header "#pragma once\n#define XF_GAME_INTERFACE_VERSION 0x${version}u\n")
# written only when they change, so what includes them is not rebuilt for
# nothing (the stamp tells the build the step ran)
function(write_if_changed path text)
    if(EXISTS ${path})
        file(READ ${path} old)
        if(old STREQUAL text)
            return()
        endif()
    endif()
    file(WRITE ${path} "${text}")
endfunction()
write_if_changed(${OUT}/xenoforge/game_interface.h "${header}")
write_if_changed(${OUT}/game_headers.inc "${inc}")
file(TOUCH ${OUT}/game_interface.stamp)
