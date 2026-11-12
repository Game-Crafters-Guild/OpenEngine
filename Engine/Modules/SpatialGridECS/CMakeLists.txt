cmake_minimum_required(VERSION 3.20)

project(SpatialGridECS LANGUAGES CXX)

# The spatial grid in a world: the mover component, the build system and the view a system queries.
add_library(SpatialGridECS OBJECT
    Source/SpatialGridBuildSystem.cpp
    Source/SpatialGridComponentInstantiations.cpp
    Source/SpatialGridView.cpp
)

target_compile_features(SpatialGridECS PUBLIC cxx_std_20)

target_include_directories(SpatialGridECS
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(SpatialGridECS
    PUBLIC
        SpatialGrid
        ECS
        Types
    PRIVATE
        # Engine is not listed: Engine.dll splices the SpatialGridECS objects (Engine/CMakeLists.txt).
        # ECSComponents gives the Engine/Include headers (Components/Transform.h, ECS/SystemScheduling.h).
        ECSComponents
        Logger
        Memory
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(SpatialGridTests
        Tests/SpatialGridBuildTests.cpp
        Tests/SpatialGridQueryTests.cpp
        Tests/SpatialGridSystemTests.cpp
    )
    # Engine only: SpatialGrid and SpatialGridECS are OBJECT modules inside Engine.dll.
    target_link_libraries(SpatialGridTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    ge_set_output_tests(SpatialGridTests)
    ge_stage_runtime_dependencies(SpatialGridTests)
    add_test(NAME SpatialGridTests COMMAND SpatialGridTests)

    # Release timings of the build and the queries (design note, section 7); built on request, not run as a test.
    add_executable(SpatialGridBenchmark EXCLUDE_FROM_ALL Benchmarks/SpatialGridBenchmark.cpp)
    target_link_libraries(SpatialGridBenchmark PRIVATE Engine)
    ge_set_output_tests(SpatialGridBenchmark)
    ge_stage_runtime_dependencies(SpatialGridBenchmark)

    # Arm D: the same benchmark over its own copy of the grid with cell keys on X and Z only. The copy's
    # definitions in this binary take the place of the ones in Engine.dll; the record's key field shows which ran.
    add_executable(SpatialGridBenchmarkPlanar EXCLUDE_FROM_ALL
        Benchmarks/SpatialGridBenchmark.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../SpatialGrid/Source/Grid.cpp
    )
    target_compile_definitions(SpatialGridBenchmarkPlanar PRIVATE GE_SPATIAL_GRID_PLANAR_KEYS=1)
    target_link_libraries(SpatialGridBenchmarkPlanar PRIVATE Engine)
    ge_set_output_tests(SpatialGridBenchmarkPlanar)
    ge_stage_runtime_dependencies(SpatialGridBenchmarkPlanar)
    # Each record line names the build commit (the generated BuildInfo/BuildCommit.h) and the configuration.
    foreach(_benchmark SpatialGridBenchmark SpatialGridBenchmarkPlanar)
        add_dependencies(${_benchmark} BuildCommit)
        target_include_directories(${_benchmark} PRIVATE "${GE_BUILD_COMMIT_INCLUDE_DIR}")
        target_compile_definitions(${_benchmark} PRIVATE GE_BUILD_CONFIG="$<CONFIG>")
    endforeach()
endif()
