# Contract test for cmake/EnginePublicInterface.cmake, run by ctest as
#   cmake -DSCRIPT=<check script> -DWORK=<scratch dir> -P <this file>
# Each case configures a scratch project (no languages) whose INTERFACE libraries
# borrow the names of Engine and its modules, then runs the check on them. The check
# fails configure, so each case is a child configure whose exit and text are read.
#
# Covered: listed definitions, a module's own Include/ directory and Engine's
# re-export of a checked module pass, a definition wrapped in a generator expression
# is named by what it defines, and a grandfathered include directory passes; an
# unlisted definition, an unlisted include directory and a re-export of a target the
# check does not cover each fail and are named in the message.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "EnginePublicInterfaceTest.cmake: SCRIPT and WORK are required")
endif()

file(REMOVE_RECURSE "${WORK}")

# configure_case(<name> <out rc> <out text> <CMake code declaring the targets>)
# The code runs in the scratch project's top directory, whose source directory
# stands in for the engine source tree; ge_check_engine_public_interface() is called
# on Engine, Logger, ECS, GameEngineRendering and Physics afterwards.
function(configure_case name rcVar textVar code)
    set(_source "${WORK}/${name}")
    file(MAKE_DIRECTORY "${_source}/Logger/Include")
    file(WRITE "${_source}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.25)\n"
        "project(EnginePublicInterfaceCase NONE)\n"
        "include(\"${SCRIPT}\")\n"
        "${code}\n"
        "ge_check_engine_public_interface(Engine Logger ECS GameEngineRendering Physics)\n")
    file(WRITE "${_source}/Logger/CMakeLists.txt"
        "add_library(Logger INTERFACE)\n"
        "target_include_directories(Logger INTERFACE \${CMAKE_CURRENT_SOURCE_DIR}/Include)\n"
        "target_compile_definitions(Logger INTERFACE LOGGER_ENABLE_FILE_LOGGING=1)\n")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -S "${_source}" -B "${_source}/build"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
    set(${rcVar} "${_rc}" PARENT_SCOPE)
    set(${textVar} "${_text}" PARENT_SCOPE)
endfunction()

function(expect_pass name code)
    configure_case(${name} _rc _text "${code}")
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${name}: the check refused a reviewed interface: ${_text}")
    endif()
endfunction()

function(expect_refused name needle code)
    configure_case(${name} _rc _text "${code}")
    if(_rc EQUAL 0)
        message(FATAL_ERROR "${name}: the check accepted an interface entry nobody reviewed")
    endif()
    foreach(_expected IN ITEMS "${needle}" "cmake/EnginePublicInterface.cmake")
        string(FIND "${_text}" "${_expected}" _at)
        if(_at LESS 0)
            message(FATAL_ERROR "${name}: the refusal does not state \"${_expected}\".\n  Message: ${_text}")
        endif()
    endforeach()
endfunction()

set(_reviewed [=[
add_subdirectory(Logger)
add_library(Engine INTERFACE)
target_compile_definitions(Engine INTERFACE GE_ENABLE_SCRIPTING=$<BOOL:ON>
    $<TARGET_PROPERTY:Logger,INTERFACE_COMPILE_DEFINITIONS>)
target_include_directories(Engine INTERFACE ${CMAKE_SOURCE_DIR}/Include
    ${CMAKE_BINARY_DIR}/generated
    $<TARGET_PROPERTY:Logger,INTERFACE_INCLUDE_DIRECTORIES>)
add_library(GameEngineRendering INTERFACE)
target_compile_definitions(GameEngineRendering INTERFACE
    $<$<CONFIG:Debug>:GE_ENABLE_DEBUG_BARRIERS=1>)
]=])

expect_pass("reviewed" "${_reviewed}")

expect_refused("unlisted-definition" "definition GE_UNREVIEWED on Logger"
    "${_reviewed}\ntarget_compile_definitions(Logger INTERFACE GE_UNREVIEWED=1)")

expect_refused("unlisted-include" "include directory <src>/Other/Include on Physics"
    "${_reviewed}\nadd_library(Physics INTERFACE)\ntarget_include_directories(Physics INTERFACE \${CMAKE_SOURCE_DIR}/Other/Include)")

expect_refused("unchecked-reexport" "on Engine ($<TARGET_PROPERTY:Unchecked"
    "${_reviewed}\nadd_library(Unchecked INTERFACE)\ntarget_compile_definitions(Engine INTERFACE \$<TARGET_PROPERTY:Unchecked,INTERFACE_COMPILE_DEFINITIONS>)")

file(REMOVE_RECURSE "${WORK}")
