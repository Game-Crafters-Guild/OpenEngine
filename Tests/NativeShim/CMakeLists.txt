# The managed ABI tests need a controllable UI host, not a running engine.
add_library(GameEngine.NativeShim SHARED
    GameEngineNativeShim.cpp
    GameEngineNativeShimUi.cpp)
target_include_directories(GameEngine.NativeShim PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Include)

# The double has the production library's name but must never overwrite its output.
set_target_properties(GameEngine.NativeShim PROPERTIES
    OUTPUT_NAME "GameEngine.Native"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES CMAKE_BUILD_TYPE)
    string(TOUPPER "${config}" configUpper)
    set_target_properties(GameEngine.NativeShim PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${configUpper} "${CMAKE_CURRENT_BINARY_DIR}/${config}"
        LIBRARY_OUTPUT_DIRECTORY_${configUpper} "${CMAKE_CURRENT_BINARY_DIR}/${config}"
        ARCHIVE_OUTPUT_DIRECTORY_${configUpper} "${CMAKE_CURRENT_BINARY_DIR}/${config}"
        PDB_OUTPUT_DIRECTORY_${configUpper} "${CMAKE_CURRENT_BINARY_DIR}/${config}")
endforeach()
