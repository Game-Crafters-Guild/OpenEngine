cmake_minimum_required(VERSION 3.20)

project(SplineECS LANGUAGES CXX)

add_library(SplineECS OBJECT
    Source/SplineService.cpp
    Source/Network/SplineNetwork.cpp
    Source/Network/SplineNetworkDetect.cpp
    Source/Systems/SplineNetworkSystem.cpp
    Source/Systems/SplineExtractionSystem.cpp
    Source/Systems/RegisterSplineSystems.cpp
)

target_compile_features(SplineECS PUBLIC cxx_std_20)

target_include_directories(SplineECS
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(SplineECS
    PUBLIC
        Spline
        ECS
        ECSComponents
        Events
        GenerationalVector
        Types
        Mathematics
        Logger
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(SplineECSTests
        Tests/SplineNetworkTests.cpp
    )
    target_link_libraries(SplineECSTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    set_target_properties(SplineECSTests PROPERTIES FOLDER "Engine/Tests")
    ge_set_output_tests(SplineECSTests)
    ge_stage_runtime_dependencies(SplineECSTests)
    add_test(NAME SplineECSTests COMMAND $<TARGET_FILE:SplineECSTests>)
endif()
