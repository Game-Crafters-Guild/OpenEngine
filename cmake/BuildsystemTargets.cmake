# BuildsystemTargets.cmake
#
# ge_collect_buildsystem_targets(<directory> <out_var>)
#
# Every target defined in <directory> and its subdirectories, for configure-time passes over
# the whole tree. Call it from a function deferred to the end of the top-level directory
# (cmake_language(DEFER)), so every subdirectory has defined its targets.

include_guard(GLOBAL)

function(ge_collect_buildsystem_targets directory out_var)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
        ge_collect_buildsystem_targets("${_subdirectory}" _nested)
        list(APPEND _targets ${_nested})
    endforeach()
    set(${out_var} ${_targets} PARENT_SCOPE)
endfunction()

# ge_is_test_executable(<target> <out_var>)
#
# TRUE for an executable that ge_set_output_tests marks or that links a GoogleTest target directly.
function(ge_is_test_executable target_name out_var)
    set(${out_var} FALSE PARENT_SCOPE)
    get_target_property(_type ${target_name} TYPE)
    if(NOT _type STREQUAL "EXECUTABLE")
        return()
    endif()
    get_target_property(_marked ${target_name} GE_TEST_EXECUTABLE)
    get_target_property(_libraries ${target_name} LINK_LIBRARIES)
    if(_marked OR "${_libraries}" MATCHES "(^|;)(GTest::|gtest|gmock)")
        set(${out_var} TRUE PARENT_SCOPE)
    endif()
endfunction()
