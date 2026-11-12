# Writes the short commit id of the checkout being built into a C++ header, run at build
# time (not configure time, which would keep the id of the last configure) as
#   cmake -DGIT=<git> -DSOURCE_DIR=<checkout> -DOUTPUT=<header> -P WriteBuildCommit.cmake
#
# The header is rewritten only when the id changes, so a build at the same commit
# recompiles nothing. A checkout git cannot read (a source archive, no git on PATH)
# writes an empty id, which the editor shows as no commit at all rather than a guess.
# The id names HEAD and ends in "-dirty" when tracked files differ from HEAD.
if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "WriteBuildCommit.cmake: SOURCE_DIR and OUTPUT are required")
endif()

set(_commit "")
if(GIT)
    execute_process(COMMAND "${GIT}" -C "${SOURCE_DIR}" rev-parse --short HEAD
                    RESULT_VARIABLE _result
                    OUTPUT_VARIABLE _output
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(_result EQUAL 0)
        set(_commit "${_output}")
        # Exit 1: tracked files differ from HEAD. Untracked files do not count.
        execute_process(COMMAND "${GIT}" -C "${SOURCE_DIR}" diff --quiet HEAD
                        RESULT_VARIABLE _dirty
                        OUTPUT_QUIET
                        ERROR_QUIET)
        if(_dirty EQUAL 1)
            string(APPEND _commit "-dirty")
        endif()
    endif()
endif()

set(_content "#pragma once\n\n// Generated at build time by cmake/WriteBuildCommit.cmake.\nnamespace GameEngine::BuildInfo\n{\n/// Short commit id at HEAD when this build ran, \"-dirty\" when tracked files had uncommitted changes; empty when git could not read it.\ninline constexpr const char* kBuildCommit = \"${_commit}\";\n} // namespace GameEngine::BuildInfo\n")

set(_existing "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" _existing)
endif()
if(NOT _existing STREQUAL _content)
    file(WRITE "${OUTPUT}" "${_content}")
endif()
