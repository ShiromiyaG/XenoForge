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
# export tables (docs/plan.md, step 1).
add_library(xenoforge STATIC
    src/aes.cpp
    src/disc.cpp
    src/exports.cpp
    src/xex.cpp
)
target_include_directories(xenoforge PUBLIC include PRIVATE src)

add_executable(xfinspect tools/xfinspect.cpp)
target_link_libraries(xfinspect PRIVATE xenoforge)

enable_testing()
foreach(name aes disc xex)
    add_executable(${name}_test tests/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE xenoforge)
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()
