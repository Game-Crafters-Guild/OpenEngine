cmake_minimum_required(VERSION 3.20)

find_package(GTest REQUIRED)

add_executable(FontAtlasCoreTests
    FontAtlasCoreTests.cpp
)

add_executable(FontAtlasShapingTests
    FontAtlasShapingTests.cpp
)

target_link_libraries(FontAtlasCoreTests
    PRIVATE
    GTest::gtest
    GTest::gtest_main
)

target_link_libraries(FontAtlasShapingTests
    PRIVATE
    GTest::gtest
    GTest::gtest_main
)

# This suite calls FT_* directly. Engine forwards its modules' INTERFACE compile
# definitions (so GE_HAVE_FREETYPE=1 arrives and the FreeType branch compiles)
# but links those modules PRIVATE, so FreeType's import library does not come
# with it. Same probe as the Text module: linked exactly when defined.
find_package(Freetype CONFIG QUIET)
if(TARGET Freetype::Freetype)
    target_link_libraries(FontAtlasShapingTests PRIVATE Freetype::Freetype)
endif()

set_target_properties(FontAtlasShapingTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

set_target_properties(FontAtlasCoreTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Output layout: bin/<Config>/Tests, with the runtime DLL closure staged
# beside each exe so the binaries run standalone. See cmake/OutputLayout.cmake.
foreach(_ge_t IN ITEMS FontAtlasCoreTests FontAtlasShapingTests)
    ge_set_output_tests(${_ge_t})
    ge_stage_runtime_dependencies(${_ge_t})
    # Engine alone: Text and Platform (whose Platform::GetExecutablePath anchors
    # the staged fixtures) are OBJECT modules inside it, and linking either beside
    # it would give the exe a second copy of its state
    # (cmake/EngineModuleDuplicationCheck.cmake).
    target_link_libraries(${_ge_t} PRIVATE Engine)
    add_custom_command(TARGET ${_ge_t} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_ge_t}>/TestData/Text"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/Apps/Editor/Assets/Fonts/Roboto-Regular.ttf"
            "${PROJECT_SOURCE_DIR}/Apps/Editor/Assets/Fonts/Roboto-Italic.ttf"
            "${CMAKE_CURRENT_SOURCE_DIR}/Fonts/NotoSansArabic-Regular.ttf"
            "${CMAKE_CURRENT_SOURCE_DIR}/Fonts/Noto-OFL.txt"
            "$<TARGET_FILE_DIR:${_ge_t}>/TestData/Text"
        VERBATIM)
endforeach()

include(GoogleTest)
add_test(NAME FontAtlasCoreTests COMMAND FontAtlasCoreTests)
add_test(NAME FontAtlasShapingTests COMMAND FontAtlasShapingTests)

