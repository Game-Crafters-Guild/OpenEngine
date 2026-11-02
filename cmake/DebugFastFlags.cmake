# ge_seed_debugfast_flags()
# Gives DebugFast the Debug compiler and linker flags; call it after project().
# CMake creates these cache entries empty when it knows DebugFast at project() (a
# single-config CMAKE_BUILD_TYPE), so an empty entry takes the Debug flags; others stay.
function(ge_seed_debugfast_flags)
    foreach(_flags IN ITEMS C_FLAGS CXX_FLAGS EXE_LINKER_FLAGS SHARED_LINKER_FLAGS STATIC_LINKER_FLAGS MODULE_LINKER_FLAGS)
        set(CMAKE_${_flags}_DEBUGFAST "${CMAKE_${_flags}_DEBUG}" CACHE STRING
            "Flags used during DEBUGFAST builds; the Debug flags when empty.")
        if("$CACHE{CMAKE_${_flags}_DEBUGFAST}" STREQUAL "")
            set_property(CACHE CMAKE_${_flags}_DEBUGFAST PROPERTY VALUE "${CMAKE_${_flags}_DEBUG}")
        endif()
    endforeach()
endfunction()
