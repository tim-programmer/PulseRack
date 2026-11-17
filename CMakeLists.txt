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

include(FetchContent)
# --- CLAP (PulseRack as a plugin in a DAW, docs/reaper.md): the headers only, pinned to a release. The plugin is built
#     with float samples only, and everything it links is compiled as position-independent code.
option(PULSERACK_CLAP "Build pulserack.clap, the rack as a CLAP instrument" ON)
if(PULSERACK_CLAP AND NOT PULSERACK_SAMPLE_TYPE STREQUAL "FLOAT")
    message(STATUS "pulserack.clap: not built (PULSERACK_SAMPLE_TYPE is ${PULSERACK_SAMPLE_TYPE}; a plugin is FLOAT)")
    set(PULSERACK_CLAP OFF)
endif()
if(PULSERACK_CLAP)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    FetchContent_Declare(clap
        URL https://github.com/free-audio/clap/archive/refs/tags/1.2.9.tar.gz
        URL_HASH SHA256=0241a9b7b3a3156c780484231885ee0ffa13fe3ff41acfcf3a32c63715926e48
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR _headers_only) # (its headers only: none of its own targets)
    FetchContent_MakeAvailable(clap)
    message(STATUS "CLAP: ${clap_SOURCE_DIR} @ 1.2.9")
endif()
# --- VST3 (docs/reaper.md): the same plugins as PulseRack.vst3, through clap-wrapper and Steinberg's VST3 SDK (MIT
#     since 3.8), both fetched and pinned to a release; only the SDK's base, pluginterfaces and public.sdk are cloned.
#     Off by default: the SDK is a large download.
option(PULSERACK_VST3 "Also build PulseRack.vst3 (clap-wrapper and the VST3 SDK, fetched)" OFF)
if(PULSERACK_VST3 AND PULSERACK_CLAP)
    FetchContent_Declare(vst3sdk
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
        GIT_TAG v3.8.0_build_66
        GIT_SHALLOW TRUE
        GIT_SUBMODULES base pluginterfaces public.sdk
        SOURCE_SUBDIR _sources_only) # (clap-wrapper compiles the parts it needs)
    FetchContent_MakeAvailable(vst3sdk)
    set(VST3_SDK_ROOT ${vst3sdk_SOURCE_DIR})
    set(CLAP_SDK_ROOT ${clap_SOURCE_DIR})
    set(CLAP_WRAPPER_OUTPUT_NAME PulseRack)
    FetchContent_Declare(clap-wrapper
        GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
        GIT_TAG v0.17.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(clap-wrapper)
    message(STATUS "VST3: clap-wrapper v0.17.0, VST3 SDK v3.8.0_build_66")
elseif(PULSERACK_VST3)
    message(STATUS "PulseRack.vst3: not built (it wraps pulserack.clap, which is not)")
endif()

# --- PulseCoreLib, fetched from GitHub (branch main for now). For work on both repositories at once, point
#     FETCHCONTENT_SOURCE_DIR_PULSECORELIB at a local checkout.
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
#     dependencies (SDL3, FreeType, HarfBuzz, FriBidi) are downloaded at the versions gpuic pins by default
#     (PULSERACK_GPUIC_DEPENDENCIES=FETCH: what CI and Windows use); SYSTEM takes them from the system, AUTO from the
#     system where they are found. A system SDL3 is found here first, so gpuic and the rack share one target.
option(PULSERACK_BUILD_GUI "Build the application (needs gpuic and SDL3)" ON)
set(PULSERACK_GPUIC_DEPENDENCIES "FETCH" CACHE STRING "gpuic's dependency policy: SYSTEM, AUTO or FETCH")
set_property(CACHE PULSERACK_GPUIC_DEPENDENCIES PROPERTY STRINGS SYSTEM AUTO FETCH)
if(PULSERACK_BUILD_GUI)
    if(PULSERACK_GPUIC_DEPENDENCIES STREQUAL "SYSTEM")
        find_package(SDL3 REQUIRED CONFIG)
    elseif(PULSERACK_GPUIC_DEPENDENCIES STREQUAL "AUTO")
        find_package(SDL3 QUIET CONFIG)
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
#     back-end is built (ALSA on Linux, which PipeWire also serves; WinMM on Windows); the PipeWire, JACK, network,
#     computer-keyboard, Windows UWP / MIDI Services and KDMAPI back-ends are off, so no further downloads (Boost,
#     readerwriterqueue, cppwinrt from NuGet) are made.
option(PULSERACK_MIDI "MIDI input (libremidi)" ON)
if(PULSERACK_MIDI)
    set(LIBREMIDI_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LIBREMIDI_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_PIPEWIRE ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_JACK ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_NETWORK ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_KEYBOARD ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_BOOST ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_WINUWP ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_WINMIDI ON CACHE BOOL "" FORCE)
    set(LIBREMIDI_NO_KDMAPI ON CACHE BOOL "" FORCE)
    FetchContent_Declare(libremidi GIT_REPOSITORY https://github.com/celtera/libremidi.git GIT_TAG v5.4.3 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(libremidi)
    message(STATUS "libremidi: ${libremidi_SOURCE_DIR} @ v5.4.3")
endif()

add_library(rack_warnings INTERFACE)
if(MSVC)
    target_compile_options(rack_warnings INTERFACE /W4 /WX /utf-8)
    # (getenv, fopen and strerror are standard C++; MSVC's "use the _s version instead" is not a warning to obey)
    target_compile_definitions(rack_warnings INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(rack_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

add_subdirectory(src)
if(PULSERACK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
