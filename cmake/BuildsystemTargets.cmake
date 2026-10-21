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
