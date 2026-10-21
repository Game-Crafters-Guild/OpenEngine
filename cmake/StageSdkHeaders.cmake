# A composite SDK header mirror owns only files its sources previously staged.
foreach(_required SOURCE_LIST DST MANIFEST_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "StageSdkHeaders: SOURCE_LIST, DST and MANIFEST_DIR are required")
    endif()
endforeach()
file(STRINGS "${SOURCE_LIST}" _current_sources)
string(SHA1 _key "${DST}")
set(_source_record "${MANIFEST_DIR}/${_key}.sources")
set(_sources "")
if(EXISTS "${_source_record}")
    file(STRINGS "${_source_record}" _sources)
endif()
list(APPEND _sources ${_current_sources})
list(REMOVE_DUPLICATES _sources)

function(prune_source source)
    set(SRC "${source}")
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PruneStagedMirrorOrphans.cmake")
endfunction()

# Prune every source before copying any: a header moved between sources must
# not be deleted by its former owner's record after its new copy.
foreach(_source IN LISTS _sources)
    prune_source("${_source}")
endforeach()
set(_copy_sources "")
foreach(_source IN LISTS _current_sources)
    if(IS_DIRECTORY "${_source}")
        list(APPEND _copy_sources "${_source}")
    endif()
endforeach()
if(_copy_sources)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different ${_copy_sources} "${DST}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()

# Keep source-root ownership when Include/ itself disappears from the collector.
# A prune that cannot remove an orphan must still be able to retry it later.
string(JOIN "\n" _source_content ${_sources})
file(CONFIGURE OUTPUT "${_source_record}" CONTENT "${_source_content}\n" @ONLY)
