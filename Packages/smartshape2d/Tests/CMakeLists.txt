find_package(GTest CONFIG REQUIRED)
add_executable(SmartShape2DTests
    ShapeGeometryTests.cpp
    ShapeAnchorTests.cpp
    EdgeQuadTests.cpp
    TextureSetTests.cpp
    ShapeOffsetTests.cpp
    ../Native/SmartShape2D/ShapeAnchor.cpp
    ../Native/SmartShape2D/EdgeQuad.cpp
    ../Native/SmartShape2D/ShapeOffset.cpp
    ../Native/SmartShape2D/ShapeMesh.cpp
    ../Native/SmartShape2D/ShapeEdges.cpp
    ../Native/clipper2/clipper.engine.cpp
    ../Native/clipper2/clipper.offset.cpp
    ../Native/clipper2/clipper.rectclip.cpp
    ShapeRuntimeTests.cpp
    ../Native/SmartShape2D/ShapeRuntime.cpp
    ShapeCollisionOwnerTests.cpp
    ../Native/SmartShape2D/ShapeCollisionOwner.cpp
    ShapeCollisionTests.cpp
    ../Native/SmartShape2D/ShapeCollision.cpp
    ../Native/SmartShape2D/ShapeGeometry.cpp)
target_compile_features(SmartShape2DTests PRIVATE cxx_std_20)
target_include_directories(SmartShape2DTests PRIVATE ../Native)
target_link_libraries(SmartShape2DTests PRIVATE Engine GTest::gtest_main)
ge_set_output_tests(SmartShape2DTests)
ge_stage_runtime_dependencies(SmartShape2DTests)
add_test(NAME SmartShape2DTests COMMAND SmartShape2DTests)
