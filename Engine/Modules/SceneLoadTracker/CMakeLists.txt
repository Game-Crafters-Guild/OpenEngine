cmake_minimum_required(VERSION 3.20)
project(SceneLoadTracker LANGUAGES CXX)

add_library(SceneLoadTracker OBJECT
    Source/SceneLoadTracker.cpp
)

target_include_directories(SceneLoadTracker
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_compile_features(SceneLoadTracker PUBLIC cxx_std_20)

target_link_libraries(SceneLoadTracker
    PUBLIC
        Events
        Types
    PRIVATE
        Logger
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(SceneLoadTrackerTests
        Tests/SceneLoadTrackerTests.cpp
    )
    target_link_libraries(SceneLoadTrackerTests
        PRIVATE
            SceneLoadTracker
            Logger
            GTest::gtest
            GTest::gtest_main
    )
    # The tests include two engine test headers: LogCapture.h and SceneLoadTrackerTestAccess.h.
    target_include_directories(SceneLoadTrackerTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Tests)
    ge_set_output_tests(SceneLoadTrackerTests)
    ge_stage_runtime_dependencies(SceneLoadTrackerTests)
    add_test(NAME SceneLoadTrackerTests COMMAND SceneLoadTrackerTests)
endif()
