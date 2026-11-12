# Contract test for the PRE_LINK step of cmake/WindowsExportAllSymbols.cmake, run by ctest as
#   cmake -DSCRIPT=<helper script> -DWORK=<scratch dir> -P <this file>
# The step writes exports.def again only when it or its record is missing, or objects.txt differs.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "WindowsExportAllSymbolsTest.cmake: SCRIPT and WORK are required")
endif()

set(_objects "${WORK}/objects.txt")
set(_def "${WORK}/exports.def")
set(_record "${WORK}/exports.def.objects")

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# run_step(): one run of the PRE_LINK step.
function(run_step)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DOBJECTS_FILE=${_objects} -DDEF_FILE=${_def} -P ${SCRIPT}
        COMMAND_ERROR_IS_FATAL ANY)
endfunction()

# expect_def_untouched(<case>): one run of the step keeps the time and the bytes of exports.def.
function(expect_def_untouched caseName)
    file(TIMESTAMP "${_def}" _timeBefore "%Y-%m-%dT%H:%M:%S.%f")
    file(SHA256 "${_def}" _hashBefore)
    run_step()
    file(TIMESTAMP "${_def}" _timeAfter "%Y-%m-%dT%H:%M:%S.%f")
    file(SHA256 "${_def}" _hashAfter)
    if(NOT _timeAfter STREQUAL _timeBefore OR NOT _hashAfter STREQUAL _hashBefore)
        message(FATAL_ERROR "${caseName}: exports.def was rewritten (${_timeBefore} -> ${_timeAfter}); the DLL would relink")
    endif()
endfunction()

# The .def exports the symbol of a removed source; the object list written after it is empty.
file(WRITE "${_def}" "EXPORTS \n\tRemovedSourceSymbol\n")
file(WRITE "${_objects}" "")
run_step()
file(READ "${_def}" _generated)
if(_generated MATCHES "RemovedSourceSymbol")
    message(FATAL_ERROR "exports.def still exports the symbol of a removed source:\n${_generated}")
endif()

# A .def that matches the object list stays as it is.
expect_def_untouched("a current exports.def")

# A regeneration writes objects.txt again with the same content and a newer time.
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
file(WRITE "${_objects}" "")
expect_def_untouched("an objects.txt with the same content and a newer time")

# A source leaves the target: objects.txt differs from the record, and the .def is written again.
file(WRITE "${_record}" "RemovedSource.obj\n")
file(WRITE "${_def}" "EXPORTS \n\tRemovedSourceSymbol\n")
run_step()
file(READ "${_def}" _generated)
if(_generated MATCHES "RemovedSourceSymbol")
    message(FATAL_ERROR "an objects.txt that differs from its record did not write exports.def again:\n${_generated}")
endif()
file(READ "${_record}" _recorded)
file(READ "${_objects}" _objectList)
if(NOT _recorded STREQUAL _objectList)
    message(FATAL_ERROR "an objects.txt that differs from its record: the record does not match the list:\n${_recorded}")
endif()

# A .def with no record, newer than the object list, is written again once and then stays.
file(REMOVE "${_record}")
file(WRITE "${_def}" "EXPORTS \n\tRemovedSourceSymbol\n")
run_step()
file(READ "${_def}" _generated)
if(_generated MATCHES "RemovedSourceSymbol")
    message(FATAL_ERROR "an exports.def with no record was not written again:\n${_generated}")
endif()
expect_def_untouched("an exports.def written again from no record")
