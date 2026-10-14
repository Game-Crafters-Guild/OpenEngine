# Audio ECS tests — AudioEmitterSystem lifecycle-consumer coverage
# (P3 #389 lifecycle events + S1 #406 SwapGenerationGuard gap recovery)
# plus AudioListener +Z-forward convention pins, and Ogg Vorbis decode and
# playback against the staged Engine/Tests/Fixtures/Audio clips.

find_package(GTest REQUIRED)

add_executable(AudioECSTests
    AudioEmitterSystemTests.cpp
    AudioListenerSystemTests.cpp
    AudioOggVorbisTests.cpp
)

# Modules are OBJECT libs aggregated into Engine.dll; link Engine alone and
# resolve module symbols across the import lib (Tests/Rendering precedent).
set_target_properties(AudioECSTests PROPERTIES UNITY_BUILD OFF)
target_compile_features(AudioECSTests PUBLIC cxx_std_20)

target_link_libraries(AudioECSTests
    PRIVATE
    Engine
    GTest::gtest
    GTest::gtest_main
)

# StagedTestPaths.h: exe-anchored fixture resolution.
target_include_directories(AudioECSTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Tests)

ge_set_output_tests(AudioECSTests)
ge_stage_runtime_dependencies(AudioECSTests)
if(TARGET StageTestAssets)
    add_dependencies(AudioECSTests StageTestAssets)
endif()

include(GoogleTest)
gtest_discover_tests(AudioECSTests)
