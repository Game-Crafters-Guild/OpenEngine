# Configures small C and C++ projects with DebugFast as their build type and reads
# their caches: the DebugFast flags must be the Debug flags, and a DebugFast flag
# value given on the command line must stay as given.
foreach(_required SCRIPT WORK GENERATOR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "DebugFastFlagsTest: SCRIPT, WORK and GENERATOR are required")
    endif()
endforeach()

set(_flagVariables C_FLAGS CXX_FLAGS EXE_LINKER_FLAGS SHARED_LINKER_FLAGS STATIC_LINKER_FLAGS MODULE_LINKER_FLAGS)
set(_source "${WORK}/source")
file(REMOVE_RECURSE "${WORK}")
file(WRITE "${_source}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.25)\n"
    "project(DebugFastFlagsCase LANGUAGES C CXX)\n"
    "include(\"${SCRIPT}\")\n"
    "ge_seed_debugfast_flags()\n")

set(_makeProgram "")
if(DEFINED MAKE_PROGRAM AND NOT MAKE_PROGRAM STREQUAL "")
    set(_makeProgram "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()

# Configures the case into <build> with the extra cache arguments.
# DebugFast is the build type of a single-config generator and the only
# configuration of a multi-config one, so CMake knows it when the languages initialize.
function(configure_case build)
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_source}" -B "${build}"
        -G "${GENERATOR}" ${_makeProgram} -DCMAKE_BUILD_TYPE=DebugFast -DCMAKE_CONFIGURATION_TYPES=DebugFast ${ARGN}
        OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
endfunction()

# Sets <out> to the value of the cache entry <name> in <build>/CMakeCache.txt.
function(read_cache_entry build name out)
    file(STRINGS "${build}/CMakeCache.txt" _lines REGEX "^${name}:[A-Z]+=")
    if(NOT _lines)
        message(FATAL_ERROR "${build}: no cache entry ${name}")
    endif()
    string(REGEX REPLACE "^${name}:[A-Z]+=" "" _value "${_lines}")
    set(${out} "${_value}" PARENT_SCOPE)
endfunction()

set(_default "${WORK}/default")
configure_case("${_default}")
foreach(_flags IN LISTS _flagVariables)
    read_cache_entry("${_default}" CMAKE_${_flags}_DEBUG _debug)
    read_cache_entry("${_default}" CMAKE_${_flags}_DEBUGFAST _debugFast)
    if(NOT _debugFast STREQUAL _debug)
        message(FATAL_ERROR "CMAKE_${_flags}_DEBUGFAST is '${_debugFast}', expected the Debug flags '${_debug}'")
    endif()
endforeach()
# The compilers' Debug flags carry the debug-information flag, so the check above is not vacuous.
foreach(_language IN ITEMS C CXX)
    read_cache_entry("${_default}" CMAKE_${_language}_FLAGS_DEBUG _debug)
    if(_debug STREQUAL "")
        message(FATAL_ERROR "CMAKE_${_language}_FLAGS_DEBUG is empty")
    endif()
endforeach()

set(_explicit "${WORK}/explicit")
configure_case("${_explicit}" "-DCMAKE_CXX_FLAGS_DEBUGFAST=-DDEBUGFAST_FROM_USER")
read_cache_entry("${_explicit}" CMAKE_CXX_FLAGS_DEBUGFAST _debugFast)
if(NOT _debugFast STREQUAL "-DDEBUGFAST_FROM_USER")
    message(FATAL_ERROR "CMAKE_CXX_FLAGS_DEBUGFAST from the command line became '${_debugFast}'")
endif()

file(REMOVE_RECURSE "${WORK}")
