# Applies PATCH (a git diff) to the current directory, once: a FetchContent PATCH_COMMAND runs again when the
# project is reconfigured, so a patch already applied is left as it is.
#   cmake -DPATCH=<file> -P apply_patch.cmake
find_package(Git REQUIRED)
execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${PATCH} RESULT_VARIABLE applied
                OUTPUT_QUIET ERROR_QUIET)
if(applied EQUAL 0)
    return()
endif()
execute_process(COMMAND ${GIT_EXECUTABLE} apply ${PATCH} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Could not apply ${PATCH}")
endif()
