# Exercises the SDK's real post-build header mirror in a small native project.
# Ordinary rebuilds after edits, deletions and renames must restage the headers.
foreach(_required SCRIPT WORK GENERATOR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "StageSdkHeadersTest: SCRIPT, WORK and GENERATOR are required")
    endif()
endforeach()

set(_source "${WORK}/source")
set(_build "${WORK}/build")
set(_sdk "${_build}/SDK")
file(REMOVE_RECURSE "${WORK}")
file(WRITE "${_source}/main.cpp" "int main() { return 0; }\n")
foreach(_mirror IN ITEMS include include-editor generated-include)
    file(WRITE "${_source}/${_mirror}/Keep.h" "keep")
    file(WRITE "${_source}/${_mirror}/Removed/Old.h" "remove")
    file(WRITE "${_source}/${_mirror}/Rename.h" "rename")
endforeach()
file(WRITE "${_source}/second/Other.h" "second source")
file(WRITE "${_source}/second/Moved.h" "move between sources")
file(WRITE "${_source}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.25)\n"
    "project(StageSdkHeadersCase LANGUAGES CXX)\n"
    "include(\"${SCRIPT}\")\n"
    "add_executable(HeaderHost main.cpp)\n"
    "foreach(mirror IN ITEMS include include-editor generated-include)\n"
    "  set(sources \"\${CMAKE_SOURCE_DIR}/\${mirror}\")\n"
    "  if(mirror STREQUAL include AND IS_DIRECTORY \"\${CMAKE_SOURCE_DIR}/second\")\n"
    "    list(APPEND sources \"\${CMAKE_SOURCE_DIR}/second\")\n"
    "  endif()\n"
    "  _ge_sdk_stage_header_mirror(HeaderHost \"\${CMAKE_BINARY_DIR}/SDK/\${mirror}\" \${sources})\n"
    "endforeach()\n")

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_source}" -B "${_build}"
    -G "${GENERATOR}" -DCMAKE_BUILD_TYPE=DebugFast -DCMAKE_CONFIGURATION_TYPES=DebugFast
    COMMAND_ERROR_IS_FATAL ANY)
function(build_headers)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_build}" --config DebugFast
        --target HeaderHost COMMAND_ERROR_IS_FATAL ANY)
endfunction()
build_headers()

foreach(_mirror IN ITEMS include include-editor generated-include)
    if(NOT EXISTS "${_sdk}/${_mirror}/Removed/Old.h")
        message(FATAL_ERROR "initial ${_mirror} staging is missing a source header")
    endif()
    file(WRITE "${_sdk}/${_mirror}/Unowned.h" "another writer")
    file(REMOVE_RECURSE "${_source}/${_mirror}/Removed")
    file(RENAME "${_source}/${_mirror}/Rename.h" "${_source}/${_mirror}/Renamed.h")
endforeach()
# Move a header from the later source to the earlier source of one mirror.
file(RENAME "${_source}/second/Moved.h" "${_source}/include/Moved.h")
build_headers()

foreach(_mirror IN ITEMS include include-editor generated-include)
    if(EXISTS "${_sdk}/${_mirror}/Removed" OR EXISTS "${_sdk}/${_mirror}/Rename.h")
        message(FATAL_ERROR "deleted or renamed header is still staged in ${_mirror}")
    endif()
    foreach(_header IN ITEMS Keep.h Renamed.h Unowned.h)
        if(NOT EXISTS "${_sdk}/${_mirror}/${_header}")
            message(FATAL_ERROR "${_mirror} lost ${_header}")
        endif()
    endforeach()
endforeach()
if(NOT EXISTS "${_sdk}/include/Other.h" OR NOT EXISTS "${_sdk}/include/Moved.h")
    message(FATAL_ERROR "one include source pruned another source's header")
endif()

# Removing the last header can remove its Include root from the collector too.
file(REMOVE_RECURSE "${_source}/second")
build_headers()
if(EXISTS "${_sdk}/include/Other.h" OR NOT EXISTS "${_sdk}/include/Moved.h")
    message(FATAL_ERROR "a vanished include root was not pruned independently")
endif()

# An edit to an otherwise unused header also reaches the mirror on the next build.
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.1 COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${_source}/include/Keep.h" "updated")
build_headers()
file(READ "${_sdk}/include/Keep.h" _content)
if(NOT _content STREQUAL "updated")
    message(FATAL_ERROR "an unused header edit did not restage")
endif()
file(GLOB_RECURSE _shipped_records "${_sdk}/*.mirror-manifest")
file(GLOB_RECURSE _build_records "${_build}/stage-stamps/*.mirror-manifest")
list(LENGTH _build_records _record_count)
if(_shipped_records OR NOT _record_count EQUAL 4)
    message(FATAL_ERROR "mirror records must live under the build tree, outside SDK")
endif()
message(STATUS "StageSdkHeadersTest: passed")
