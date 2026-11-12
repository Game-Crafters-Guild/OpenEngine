# ge_windows_export_all_symbols(<target>): exports every symbol of the SHARED library <target>.
# Under Visual Studio, a PRE_LINK step writes exports.def again when objects.txt differs in
# content from its record (exports.def.objects), so the .def never keeps a removed source.
function(ge_windows_export_all_symbols target)
    set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
        return()
    endif()
    set(objectDir "$<TARGET_PROPERTY:${target},BINARY_DIR>/${target}.dir/$<CONFIG>")
    add_custom_command(TARGET ${target} PRE_LINK
        COMMAND ${CMAKE_COMMAND}
            "-DOBJECTS_FILE=${objectDir}/objects.txt"
            "-DDEF_FILE=${objectDir}/exports.def"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        VERBATIM)
endfunction()

# Entry point for the PRE_LINK step. `cmake -E __create_def` keeps a .def that is newer than
# each object, so the step removes the .def first. The record is written only after a success.
if(CMAKE_SCRIPT_MODE_FILE)
    set(recordFile "${DEF_FILE}.objects")
    file(READ "${OBJECTS_FILE}" objectList)
    if(EXISTS "${DEF_FILE}" AND EXISTS "${recordFile}")
        file(READ "${recordFile}" recordedObjectList)
        if(recordedObjectList STREQUAL objectList)
            return()
        endif()
    endif()
    file(REMOVE "${recordFile}" "${DEF_FILE}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E __create_def "${DEF_FILE}" "${OBJECTS_FILE}"
        RESULT_VARIABLE createResult)
    if(NOT createResult EQUAL 0)
        message(FATAL_ERROR "WindowsExportAllSymbols: `cmake -E __create_def` could not write ${DEF_FILE} "
            "from ${OBJECTS_FILE} (${createResult}). Make sure every object in the list exists, then build again.")
    endif()
    file(WRITE "${recordFile}" "${objectList}")
endif()
