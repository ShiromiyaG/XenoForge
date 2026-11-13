# Cross-compiling for ARM64 Linux with clang, on an x86-64 Linux host
# (docs/runtime.md, "Other hosts"): the cross libstdc++ and C library from the
# distribution (Debian and Ubuntu: g++-aarch64-linux-gnu), and qemu-user to
# run what the build itself runs (xfrecomp, xffuzz) and the tests.
#
#   cmake -S . -B build-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake
#   cmake --build build-arm64 --target ppc_test ppc_recompiled_test ppc_fuzz_test
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(XF_CROSS_TRIPLE aarch64-linux-gnu)
set(XF_CROSS_SYSROOT /usr/${XF_CROSS_TRIPLE} CACHE PATH "the cross C library and libstdc++")

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${XF_CROSS_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${XF_CROSS_TRIPLE})
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")

find_program(XF_QEMU_AARCH64 qemu-aarch64)
if(XF_QEMU_AARCH64)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${XF_QEMU_AARCH64} -L ${XF_CROSS_SYSROOT})
endif()

set(CMAKE_FIND_ROOT_PATH ${XF_CROSS_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
