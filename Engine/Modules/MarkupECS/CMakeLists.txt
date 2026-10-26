cmake_minimum_required(VERSION 3.20)

project(MarkupECS LANGUAGES CXX)

# World mark-ups: the notes store, the tag vocabulary and the revision beside the
# Markup components, and a region's outline rules and area (MarkupRegionOutline,
# MarkupRegionArea, and the footprints as rings for the area boolean its display draws,
# MarkupRegionBoolean, over SplineGeometry's CombineAreas and BuildAreaGround).
# Editor-authored data; a game export strips the components
# (Engine/Build/SceneExportStrip.h).
add_library(MarkupECS OBJECT
    Source/MarkupComponentInstantiations.cpp
    Source/MarkupRegionArea.cpp
    Source/MarkupRegionBoolean.cpp
    Source/MarkupRegionDisplayCache.cpp
    Source/MarkupRegionMesh.cpp
    Source/MarkupRegionOutline.cpp
    Source/MarkupService.cpp
    Source/Scene/MarkupRegionSceneSchema.cpp
    Source/Scene/MarkupSceneSchema.cpp
)

target_compile_features(MarkupECS PUBLIC cxx_std_20)

target_include_directories(MarkupECS
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(MarkupECS
    PUBLIC
        ECS
        ECSComponents
        Mathematics
        Spline
        SplineECS
        SplineGeometry
        Types
    PRIVATE
        # MarkupSceneSchema.cpp includes Scene/SceneSchemaRegistry.h, which
        # includes AssetCore headers.
        AssetCore
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(MarkupTests
        Tests/MarkupRegionBooleanTests.cpp
        Tests/MarkupRegionMeshTests.cpp
        Tests/MarkupRegionTests.cpp
        Tests/MarkupServiceTests.cpp
        Tests/MarkupSceneSchemaTests.cpp
    )
    target_link_libraries(MarkupTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    set_target_properties(MarkupTests PROPERTIES FOLDER "Engine/Tests")
    ge_set_output_tests(MarkupTests)
    ge_stage_runtime_dependencies(MarkupTests)
    add_test(NAME MarkupTests COMMAND $<TARGET_FILE:MarkupTests>)
endif()
