add_library(rack_engine STATIC
    engine/model/registry.cpp
    engine/model/patch.cpp
    engine/model/plan.cpp
    engine/model/patchfile.cpp
    engine/rt/modules_lib.cpp
    engine/rt/modules_glue.cpp
    engine/engine.cpp
)
target_include_directories(rack_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rack_engine PUBLIC pcl::pcl PRIVATE rack_warnings)

add_executable(rack-render render_main.cpp)
target_link_libraries(rack-render PRIVATE rack_engine rack_warnings)

# The rack's geometry (panel layout, knob mapping, placement): no window, so the headless tests use it.
add_library(rack_geometry STATIC ui/geometry.cpp)
target_link_libraries(rack_geometry PUBLIC rack_engine PRIVATE rack_warnings)

# The application (milestone 2: the window, without cables or sound).
if(PULSERACK_BUILD_GUI)
    add_library(rack_ui STATIC ui/rack_ui.cpp)
    target_link_libraries(rack_ui PUBLIC rack_geometry gpuic::gpuic gpuic::cpp PRIVATE rack_warnings)
    add_executable(pulserack main.cpp)
    target_link_libraries(pulserack PRIVATE rack_ui rack_warnings)
endif()
