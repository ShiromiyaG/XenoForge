# A new port's repository from the SDK's template (docs/sdk.md, "Starting a port"):
#
#   cmake -DNAME=<name> [-DTITLE=<the game's title>] [-DDIR=<directory>] [-DISO=<disc image>]
#         -P <the SDK's prefix>/share/xenoforge/new_port.cmake
#
# NAME is the port's identifier (letters, digits, _; the targets are
# player_<NAME>, boot_<NAME>, <NAME>_hooks); TITLE the window's title (NAME by
# default); DIR where the repository goes (./<NAME> by default; it must be
# empty or not exist). Writes CMakeLists.txt, CMakePresets.json, project.toml,
# src/main.cpp, src/hooks.cpp, README.md, .gitignore, and CMakeUserPresets.json
# with this machine's SDK prefix (and ISO, when given): `cmake --preset local`
# then builds it, with this host's preset (windows or linux). The port is
# made for Windows, Linux (x86-64, ARM64) and Android (arm64-v8a): its
# CMakePresets.json has a preset for each. Run from XenoForge's source tree (sdk/new_port.cmake) the SDK
# prefix is left for the user to fill in.

if(NOT NAME)
    message(FATAL_ERROR "new_port: -DNAME=<name> is needed (letters, digits and _)")
endif()
if(NOT NAME MATCHES "^[A-Za-z][A-Za-z0-9_]*$")
    message(FATAL_ERROR "new_port: NAME=${NAME}: letters, digits and _, starting with a letter")
endif()
if(NOT TITLE)
    set(TITLE ${NAME})
endif()
if(NOT DIR)
    set(DIR ${CMAKE_CURRENT_BINARY_DIR}/${NAME})
endif()
get_filename_component(DIR "${DIR}" ABSOLUTE)
if(EXISTS "${DIR}")
    file(GLOB present "${DIR}/*" "${DIR}/.*")
    list(FILTER present EXCLUDE REGEX "/\\.git$")
    if(present)
        message(FATAL_ERROR "new_port: ${DIR} is not empty")
    endif()
endif()

# the template beside this script: share/xenoforge/template installed, sdk/template in the source tree
set(template ${CMAKE_CURRENT_LIST_DIR}/template)
if(NOT EXISTS ${template}/CMakeLists.txt.in)
    message(FATAL_ERROR "new_port: no template in ${template}")
endif()
# the SDK's prefix and version, when this is the installed script (<prefix>/share/xenoforge)
set(PORT_SDK "")
set(XENOFORGE_VERSION "")
get_filename_component(prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(EXISTS ${prefix}/lib/cmake/XenoForge/XenoForgeConfigVersion.cmake)
    set(PORT_SDK ${prefix})
    file(STRINGS ${prefix}/lib/cmake/XenoForge/XenoForgeConfigVersion.cmake line REGEX "^set\\(PACKAGE_VERSION \"")
    string(REGEX REPLACE "^set\\(PACKAGE_VERSION \"([^\"]*)\".*$" "\\1" XENOFORGE_VERSION "${line}")
endif()
# the presets this machine builds with (CMakePresets.json's windows or linux)
if(CMAKE_HOST_WIN32)
    set(PORT_HOST_PRESET windows)
else()
    set(PORT_HOST_PRESET linux)
endif()
set(PORT_NAME ${NAME})
set(PORT_TITLE ${TITLE})
set(PORT_ISO "")
if(ISO)
    get_filename_component(PORT_ISO "${ISO}" ABSOLUTE)
endif()
# JSON wants forward slashes (or escaped backslashes)
string(REPLACE "\\" "/" PORT_SDK "${PORT_SDK}")
string(REPLACE "\\" "/" PORT_ISO "${PORT_ISO}")

foreach(file CMakeLists.txt CMakePresets.json CMakeUserPresets.json project.toml README.md src/main.cpp src/hooks.cpp)
    configure_file(${template}/${file}.in ${DIR}/${file} @ONLY NEWLINE_STYLE LF)
endforeach()
configure_file(${template}/gitignore.in ${DIR}/.gitignore @ONLY NEWLINE_STYLE LF)

message(STATUS "new_port: ${TITLE} (${NAME}) in ${DIR}")
if(PORT_SDK)
    message(STATUS "new_port: the SDK at ${PORT_SDK}")
else()
    message(STATUS "new_port: set XENOFORGE_SDK in CMakeUserPresets.json to the SDK's prefix")
endif()
if(NOT PORT_ISO)
    message(STATUS "new_port: set GAME_ISO in CMakeUserPresets.json to the disc image")
endif()
message(STATUS "new_port: then cmake --preset local && cmake --build --preset local")
