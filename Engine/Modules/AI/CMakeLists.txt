cmake_minimum_required(VERSION 3.20)

project(AI LANGUAGES CXX)

# The decision core: response curves, utility option sets, memory records and the state tree. No World, no ECS.
add_library(AI OBJECT
    Source/OptionSet.cpp
    Source/RecordStore.cpp
    Source/ResponseCurve.cpp
    Source/StateTree/StateTreeInterpreter.cpp
    Source/StateTree/StateTreeProgram.cpp
)

target_compile_features(AI PUBLIC cxx_std_20)

target_include_directories(AI
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_link_libraries(AI
    PUBLIC
        Types
        Mathematics
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(AiCoreTests
        Tests/OptionSetTests.cpp
        Tests/RecordStoreTests.cpp
        Tests/ResponseCurveTests.cpp
        Tests/StateTreeInterpreterTests.cpp
    )
    target_link_libraries(AiCoreTests PRIVATE
        AI
        GTest::gtest
        GTest::gtest_main
    )
    ge_set_output_tests(AiCoreTests)
    ge_stage_runtime_dependencies(AiCoreTests)
    add_test(NAME AiCoreTests COMMAND AiCoreTests)
endif()
