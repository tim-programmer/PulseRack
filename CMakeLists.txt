cmake_minimum_required(VERSION 3.20)
project(PulseRack VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PULSERACK_SAMPLE_TYPE "FLOAT" CACHE STRING "Sample type of the whole rack: FLOAT, Q15 or Q31")
set_property(CACHE PULSERACK_SAMPLE_TYPE PROPERTY STRINGS FLOAT Q15 Q31)
if(NOT PULSERACK_SAMPLE_TYPE MATCHES "^(FLOAT|Q15|Q31)$")
    message(FATAL_ERROR "PULSERACK_SAMPLE_TYPE must be FLOAT, Q15 or Q31 (got '${PULSERACK_SAMPLE_TYPE}')")
endif()
option(PULSERACK_BUILD_TESTS "Build the engine tests" ON)

# --- PulseCoreLib, fetched from GitHub (branch main for now). For work on both repositories at once, point
#     FETCHCONTENT_SOURCE_DIR_PULSECORELIB at a local checkout.
include(FetchContent)
set(PCL_SAMPLE_TYPE "${PULSERACK_SAMPLE_TYPE}" CACHE STRING "" FORCE)
set(PCL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PCL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PCL_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(pulsecorelib GIT_REPOSITORY https://github.com/tim-programmer/PulseCoreLib.git GIT_TAG main)
FetchContent_MakeAvailable(pulsecorelib)
execute_process(COMMAND git -C ${pulsecorelib_SOURCE_DIR} rev-parse --short HEAD
                OUTPUT_VARIABLE pcl_rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
message(STATUS "PulseCoreLib: ${pulsecorelib_SOURCE_DIR} @ ${pcl_rev} (sample type ${PULSERACK_SAMPLE_TYPE})")

add_library(rack_warnings INTERFACE)
if(MSVC)
    target_compile_options(rack_warnings INTERFACE /W4 /WX)
else()
    target_compile_options(rack_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

add_subdirectory(src)
if(PULSERACK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
