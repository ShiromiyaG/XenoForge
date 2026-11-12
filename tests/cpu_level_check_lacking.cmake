# cpu_level_check_test on a CPU that pretends to lack the level its "generated
# code" was compiled for (XF_HOST_CPU_LEVEL=none): the check's message and exit
# code (xenoforge/cpu_level_check.h's kCpuLevelExitCode), before the game's
# initializer and main.
#   cmake -DPROGRAM=<cpu_level_check_test> -P cpu_level_check_lacking.cmake
set(ENV{XF_HOST_CPU_LEVEL} none)
execute_process(COMMAND ${PROGRAM} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 86)
    message(FATAL_ERROR "exit ${result}, not 86\n${out}${err}")
endif()
if(NOT err MATCHES "It cannot run here")
    message(FATAL_ERROR "no message on the standard error\n${out}${err}")
endif()
if(out MATCHES "ran")
    message(FATAL_ERROR "code ran past the check\n${out}")
endif()
message(STATUS "stopped: ${err}")
