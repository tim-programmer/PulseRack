file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
add_executable(rack_tests ${TEST_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(rack_tests PRIVATE rack_engine rack_geometry rack_midi_core rack_warnings Threads::Threads)
target_compile_definitions(rack_tests PRIVATE RACK_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_include_directories(rack_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME rack_tests COMMAND rack_tests)

# The example patches, rendered by the real rack-render binary: it must load them without warnings and make sound.
foreach(ex sequence arpeggio filter_sweep kick_drum chill pop edm metal drum_machine voices)
    add_test(NAME render_${ex}
             COMMAND rack-render ${PROJECT_SOURCE_DIR}/examples/${ex}.rack ${CMAKE_CURRENT_BINARY_DIR}/${ex}.wav 6)
    set_tests_properties(render_${ex} PROPERTIES PASS_REGULAR_EXPRESSION "peak 0\\.[0-9]*[1-9][0-9]*, rms 0\\.(00[1-9]|0[1-9]|[1-9])"
                         FAIL_REGULAR_EXPRESSION "warning")
endforeach()

# The window (needs a display; skipped without one): driven by synthetic input, and a screenshot of a
# loaded example patch that must not be blank. (The time limits are for a hang: with a software renderer on a CI
# runner ui_smoke takes about 35 s on Linux and over a minute on Windows.)
if(PULSERACK_BUILD_GUI)
    add_executable(ui_smoke ui_smoke.cpp)
    target_link_libraries(ui_smoke PRIVATE rack_ui rack_warnings)
    add_test(NAME ui_smoke COMMAND ui_smoke)
    set_tests_properties(ui_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    # with the sound card on, through SDL's dummy driver (no real device is needed or used)
    add_test(NAME ui_sound COMMAND ui_smoke --sound)
    set_tests_properties(ui_sound PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    # files: New, Open, Save, the question, examples and recovery, with the test's own file chooser
    add_test(NAME ui_files COMMAND ui_smoke --files)
    set_tests_properties(ui_files PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    add_test(NAME ui_undo COMMAND ui_smoke --undo)
    set_tests_properties(ui_undo PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    add_test(NAME ui_steps COMMAND ui_smoke --steps)
    set_tests_properties(ui_steps PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    add_test(NAME ui_gaps COMMAND ui_smoke --gaps)
    set_tests_properties(ui_gaps PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)

    add_test(NAME ui_screenshot COMMAND pulserack ${PROJECT_SOURCE_DIR}/examples/sequence.rack)
    set_tests_properties(ui_screenshot PROPERTIES FIXTURES_SETUP shot TIMEOUT 30
        ENVIRONMENT "GPUIC_SCREENSHOT=${CMAKE_CURRENT_BINARY_DIR}/sequence.ppm;GPUIC_WAIT_MS=300;GPUIC_IGNORE_REAL_MOUSE=1;SDL_AUDIO_DRIVER=dummy")
    add_test(NAME ui_screenshot_not_blank COMMAND ui_smoke --check-ppm ${CMAKE_CURRENT_BINARY_DIR}/sequence.ppm)
    set_tests_properties(ui_screenshot_not_blank PROPERTIES FIXTURES_REQUIRED shot)
endif()
