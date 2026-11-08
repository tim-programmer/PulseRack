file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
add_executable(rack_tests ${TEST_SOURCES})
target_link_libraries(rack_tests PRIVATE rack_engine rack_warnings)
target_include_directories(rack_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME rack_tests COMMAND rack_tests)

# The example patches, rendered by the real rack-render binary: it must load them without warnings and make sound.
foreach(ex sequence arpeggio filter_sweep)
    add_test(NAME render_${ex}
             COMMAND rack-render ${CMAKE_SOURCE_DIR}/examples/${ex}.rack ${CMAKE_CURRENT_BINARY_DIR}/${ex}.wav 6)
    set_tests_properties(render_${ex} PROPERTIES PASS_REGULAR_EXPRESSION "peak 0\\.[0-9]*[1-9][0-9]*, rms 0\\.(00[1-9]|0[1-9]|[1-9])"
                         FAIL_REGULAR_EXPRESSION "warning")
endforeach()
