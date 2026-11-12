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

# --- gpuic (the window), fetched the same way; FETCHCONTENT_SOURCE_DIR_GPUIC points at a local checkout. Its own
#     dependencies (SDL3, FreeType, HarfBuzz, FriBidi) come from the system unless PULSERACK_GPUIC_DEPENDENCIES=AUTO,
#     so a build never downloads them unasked. SDL3 is found first so gpuic and the rack share one target.
option(PULSERACK_BUILD_GUI "Build the application (needs gpuic and SDL3)" ON)
set(PULSERACK_GPUIC_DEPENDENCIES "SYSTEM" CACHE STRING "gpuic's dependency policy: SYSTEM or AUTO")
set_property(CACHE PULSERACK_GPUIC_DEPENDENCIES PROPERTY STRINGS SYSTEM AUTO)
if(PULSERACK_BUILD_GUI)
    if(PULSERACK_GPUIC_DEPENDENCIES STREQUAL "SYSTEM")
        find_package(SDL3 REQUIRED CONFIG)
    endif()
    set(GPUIC_DEPENDENCIES "${PULSERACK_GPUIC_DEPENDENCIES}" CACHE STRING "" FORCE)
    set(GPUIC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GPUIC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GPUIC_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(gpuic GIT_REPOSITORY https://github.com/tim-programmer/gpuic.git GIT_TAG main)
    FetchContent_MakeAvailable(gpuic)
    execute_process(COMMAND git -C ${gpuic_SOURCE_DIR} rev-parse --short HEAD
                    OUTPUT_VARIABLE gpuic_rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    message(STATUS "gpuic: ${gpuic_SOURCE_DIR} @ ${gpuic_rev}")
endif()

# --- libremidi (MIDI input, docs/midi.md), fetched and pinned to a release tag: a third-party library does not change
#     under us between builds. FETCHCONTENT_SOURCE_DIR_LIBREMIDI points at a local checkout. Only the platform's own
#     back-end is built (ALSA on Linux, which PipeWire also serves); the PipeWire, JACK, network and computer-keyboard
#     back-ends are off, so no further downloads (Boost, readerwriterqueue) are made.
option(PULSERACK_MIDI "MIDI input (libremidi)" ON)
if(PULSERACK_MIDI)
    set(LIBREMIDI_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LIBREMIDI_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_PIPEWIRE ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_JACK ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_NETWORK ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_KEYBOARD ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_BOOST ON CACHE BOOL "" FORCE)
    FetchContent_Declare(libremidi GIT_REPOSITORY https://github.com/celtera/libremidi.git GIT_TAG v5.4.3 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(libremidi)
    message(STATUS "libremidi: ${libremidi_SOURCE_DIR} @ v5.4.3")
endif()

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
