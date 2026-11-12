# ThreadSanitizerOptions.cmake
#
# Compiles Tests/ThreadSanitizerOptions.cpp (the runtime's default options and suppressions) into every
# test executable, so a direct run and a ctest run get the same options. Include it only when ENABLE_TSAN
# is ON. The pass is deferred to the end of the top-level directory, as in AllocationHook.cmake.

include(${CMAKE_CURRENT_LIST_DIR}/BuildsystemTargets.cmake)

set(GE_TSAN_OPTIONS_SOURCE "${CMAKE_SOURCE_DIR}/Tests/ThreadSanitizerOptions.cpp")

function(ge_add_tsan_options_to_test_executables)
    ge_collect_buildsystem_targets("${CMAKE_SOURCE_DIR}" _targets)
    set(_count 0)
    foreach(_target IN LISTS _targets)
        ge_is_test_executable(${_target} _is_test)
        if(_is_test)
            target_sources(${_target} PRIVATE ${GE_TSAN_OPTIONS_SOURCE})
            math(EXPR _count "${_count} + 1")
        endif()
    endforeach()
    message(STATUS "ThreadSanitizer: default options compiled into ${_count} test executables")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL ge_add_tsan_options_to_test_executables)
