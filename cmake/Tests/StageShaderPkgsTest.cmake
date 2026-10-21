if(NOT DEFINED SCRIPT OR NOT DEFINED WORK OR NOT DEFINED EXTENSION)
    message(FATAL_ERROR "StageShaderPkgsTest: SCRIPT, WORK and EXTENSION are required")
endif()

set(_src "${WORK}/source")
set(_dst "${WORK}/staged")
set(_name "shader.${EXTENSION}")

function(stage _expect_success)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DSRC_DIR=${_src}" "-DDST_DIR=${_dst}" -P "${SCRIPT}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
    if(_expect_success)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "staging failed (${_result}): ${_output}${_error}")
        endif()
        file(READ "${_dst}/${_name}" _staged)
        if(NOT _staged STREQUAL "fresh shader")
            message(FATAL_ERROR "staging succeeded without the fresh shader: ${_staged}")
        endif()
    elseif(_result EQUAL 0 OR NOT _error MATCHES "StageShaderPkgs:.*${_name}")
        message(FATAL_ERROR "failed copy must fail staging and name the shader (${_result}): ${_output}${_error}")
    endif()
endfunction()

# An invalid source cannot be copied even after the destination is renamed.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${_src}/${_name}")
file(WRITE "${_dst}/${_name}" "stale shader")
stage(FALSE)

# With no destination to rename, the first failed copy must also be fatal.
file(REMOVE_RECURSE "${_dst}")
stage(FALSE)

file(REMOVE_RECURSE "${WORK}")
file(WRITE "${_src}/${_name}" "fresh shader")
file(WRITE "${_src}/unrelated.txt" "not a shader")
file(WRITE "${_dst}/${_name}" "stale shader")
file(WRITE "${_dst}/${_name}.stale-old" "old rename-aside copy")
stage(TRUE)
if(EXISTS "${_dst}/unrelated.txt" OR EXISTS "${_dst}/${_name}.stale-old")
    message(FATAL_ERROR "staging copied an unrelated file or retained an unlocked stale copy")
endif()

# An identical shader is not rewritten by the every-build staging step.
file(TIMESTAMP "${_dst}/${_name}" _before "%s")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.1 COMMAND_ERROR_IS_FATAL ANY)
stage(TRUE)
file(TIMESTAMP "${_dst}/${_name}" _after "%s")
if(NOT _before STREQUAL _after)
    message(FATAL_ERROR "identical shader was rewritten")
endif()

# A directory at the destination makes COPY_FILE fail on every platform, while
# rename-aside can recover. This exercises recovery without an OS-specific lock.
file(REMOVE "${_dst}/${_name}")
file(MAKE_DIRECTORY "${_dst}/${_name}")
stage(TRUE)

message(STATUS "StageShaderPkgsTest (${EXTENSION}): passed")
