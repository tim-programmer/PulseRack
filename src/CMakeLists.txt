add_library(rack_engine STATIC
    engine/model/registry.cpp
    engine/model/patch.cpp
    engine/model/plan.cpp
    engine/model/patchfile.cpp
    engine/model/wav.cpp
    engine/rt/modules_lib.cpp
    engine/rt/modules_glue.cpp
    engine/rt/modules_fx.cpp
    engine/rt/modules_drums.cpp
    engine/engine.cpp
)
target_include_directories(rack_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rack_engine PUBLIC pcl::pcl PRIVATE rack_warnings)

add_executable(rack-render render_main.cpp)
target_link_libraries(rack-render PRIVATE rack_engine rack_warnings)

# The rack's geometry (panel layout, knob mapping, placement) and patch files: no window, so the headless tests use them.
# The factory patches: examples/*.rack compiled in as text, so the application needs no path to them.
# (A changed or new .rack file reconfigures, so the copy is never stale.)
file(GLOB RACK_FACTORY_PATCHES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/examples/*.rack)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RACK_FACTORY_PATCHES})
set(rack_factory_body "")
foreach(f ${RACK_FACTORY_PATCHES})
    get_filename_component(name ${f} NAME_WE)
    file(READ ${f} text)
    string(REPLACE "_" " " title "${name}")
    string(SUBSTRING "${title}" 0 1 first)
    string(TOUPPER "${first}" first)
    string(SUBSTRING "${title}" 1 -1 rest)
    string(REGEX MATCH "^# ([^\n]*)" _ "${text}")
    set(about "${CMAKE_MATCH_1}")
    string(APPEND rack_factory_body "    { \"${name}\", \"${first}${rest}\", R\"rack(${about})rack\", R\"rack(${text})rack\" },\n")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/factory_patches.cpp.in
"// Generated from examples/*.rack by src/CMakeLists.txt: do not edit.
#include \"ui/files.h\"

namespace rack::ui
{
static const FactoryPatch kFactory[] = {
${rack_factory_body}};
std::span<const FactoryPatch> factory_patches() { return kFactory; }
} // namespace rack::ui
")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/factory_patches.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/factory_patches.cpp COPYONLY)

add_library(rack_geometry STATIC ui/geometry.cpp ui/files.cpp ui/settings.cpp ${CMAKE_CURRENT_BINARY_DIR}/factory_patches.cpp)
target_link_libraries(rack_geometry PUBLIC rack_engine PRIVATE rack_warnings)

# MIDI (docs/midi.md): the messages, the inboxes and the device names need no libremidi, so the tests always build
# them; the devices themselves (inputs.cpp, the only file that includes libremidi) and the pulserack-midi monitor
# only with PULSERACK_MIDI.
add_library(rack_midi_core STATIC midi/message.cpp midi/names.cpp)
target_include_directories(rack_midi_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rack_midi_core PRIVATE rack_warnings)
if(PULSERACK_MIDI)
    add_library(rack_midi STATIC midi/inputs.cpp)
    target_link_libraries(rack_midi PUBLIC rack_midi_core PRIVATE libremidi rack_warnings)
    add_executable(pulserack-midi midi_monitor.cpp)
    target_link_libraries(pulserack-midi PRIVATE rack_midi rack_warnings)
endif()

# The application: the window, cables and sound.
if(PULSERACK_BUILD_GUI)
    # the sound card (milestone 4): SDL3 audio, which gpuic already brings
    add_library(rack_audio STATIC audio/audio_out.cpp)
    target_include_directories(rack_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(rack_audio PUBLIC rack_engine SDL3::SDL3 PRIVATE rack_warnings)
    add_library(rack_ui STATIC ui/rack_ui.cpp ui/dialog.cpp)
    target_link_libraries(rack_ui PUBLIC rack_geometry rack_audio gpuic::gpuic gpuic::cpp PRIVATE rack_warnings)
    if(PULSERACK_MIDI)
        target_link_libraries(rack_ui PUBLIC rack_midi)
        target_compile_definitions(rack_ui PUBLIC PULSERACK_MIDI=1)
    endif()
    add_executable(pulserack main.cpp)
    target_link_libraries(pulserack PRIVATE rack_ui rack_warnings)
endif()
