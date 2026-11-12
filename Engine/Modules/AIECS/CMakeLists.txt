cmake_minimum_required(VERSION 3.20)

project(AIECS LANGUAGES CXX)

# The AI in a world: the agent components, the registry and the four AI systems.
add_library(AIECS OBJECT
    Source/AiComponentInstantiations.cpp
    Source/Assets/AiJsonFields.cpp
    Source/Assets/AiOptionSetData.cpp
    Source/Assets/AiStateTreeData.cpp
    Source/AiGroup/AiGroupKnowledge.cpp
    Source/AiGroup/AiGroupStore.cpp
    Source/Events/AiEventQueue.cpp
    Source/AiRegistry.cpp
    Source/AiTaskContext.cpp
    Source/AiTestWorld.cpp
    Source/Scene/AiAgentSceneSchema.cpp
    Source/Scene/AiCellsSceneSchema.cpp
    Source/Scene/AiGroupSceneSchema.cpp
    Source/Scene/AiSceneText.cpp
    Source/StateTree/AiStateTreeDecider.cpp
    Source/Systems/AiApplySystem.cpp
    Source/Systems/AiDecideSystem.cpp
    Source/Systems/AiKnowledgeSystem.cpp
    Source/Systems/AiTickSystem.cpp
)

target_compile_features(AIECS PUBLIC cxx_std_20)

target_include_directories(AIECS
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(AIECS
    PUBLIC
        AI
        AssetCore
        ECS
        GenerationalVector
        Types
    PRIVATE
        nlohmann_json::nlohmann_json
        Serialization
        # Engine is not listed: Engine.dll splices the AIECS objects (Engine/CMakeLists.txt).
        # ECSComponents gives the Engine/Include headers (ECS/SystemScheduling.h).
        ECSComponents
        JobSystem
        Logger
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(AiEcsTests
        Tests/AiAgentLayoutTests.cpp
        Tests/AiCellsTests.cpp
        Tests/AiDecideTests.cpp
        Tests/AiDeterminismTests.cpp
        Tests/AiEventTests.cpp
        Tests/AiKnowledgeTests.cpp
        Tests/AiModuleRegistrationTests.cpp
        Tests/AiOptionSetAssetTests.cpp
        Tests/AiRegistryTests.cpp
        Tests/AiSaveTests.cpp
        Tests/AiScheduleTests.cpp
        Tests/AiStateTreeAssetTests.cpp
        Tests/AiStateTreeBindingTests.cpp
        Tests/AiStateTreeReloadTests.cpp
        Tests/AiTraceTests.cpp
    )
    # Engine only: AIECS and AI are OBJECT modules inside Engine.dll; a second link would give
    # the executable a second AiRegistry.
    target_link_libraries(AiEcsTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    target_include_directories(AiEcsTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Tests  # EngineLogCapture.h
        ${CMAKE_SOURCE_DIR}/Tests)        # TestTempDir.h
    ge_set_output_tests(AiEcsTests)
    ge_stage_runtime_dependencies(AiEcsTests)
    add_test(NAME AiEcsTests COMMAND AiEcsTests)

    # Release timings of the decision loop; built on request, not run as a test.
    add_executable(AiTickBenchmark EXCLUDE_FROM_ALL Benchmarks/AiTickBenchmark.cpp)
    target_link_libraries(AiTickBenchmark PRIVATE Engine)
    ge_set_output_tests(AiTickBenchmark)
    ge_stage_runtime_dependencies(AiTickBenchmark)
endif()
