cmake_minimum_required(VERSION 3.20)

project(Terrain LANGUAGES CXX)

add_library(Terrain OBJECT
    Source/Heightfield.cpp
    Source/CDLODQuadtree.cpp
    Source/CDLODSelection.cpp
)

target_compile_features(Terrain PUBLIC cxx_std_20)

target_include_directories(Terrain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(Terrain
    PUBLIC
        Types
        Mathematics
        Logger
)

# Image: the 16-bit PNG heightmap decode.
target_link_libraries(Terrain PRIVATE Image)

# The root CMakeLists finds GTest AFTER this module is added, so resolve it
# here — otherwise the guard below never passes and TerrainTests silently
# drops out of the build.
if(BUILD_TESTING AND NOT TARGET GTest::gtest)
    find_package(GTest CONFIG QUIET)
endif()

if(BUILD_TESTING AND (GTest_FOUND OR TARGET GTest::gtest))
    add_executable(TerrainTests
        Tests/HeightfieldTests.cpp
        Tests/CDLODSelectionTests.cpp
    )
    # Image and Logger are OBJECT libraries: their objects reach the executable only
    # when it links them directly.
    target_link_libraries(TerrainTests PRIVATE Terrain Image Logger GTest::gtest GTest::gtest_main)
    set_target_properties(TerrainTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests/Terrain"
    )
    ge_set_output_tests(TerrainTests)
    ge_stage_runtime_dependencies(TerrainTests)
    add_test(NAME TerrainTests COMMAND TerrainTests)
endif()
