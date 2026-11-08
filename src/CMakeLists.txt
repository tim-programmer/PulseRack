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
