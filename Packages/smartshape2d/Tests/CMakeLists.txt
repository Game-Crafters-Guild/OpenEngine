find_package(GTest CONFIG REQUIRED)
add_executable(SmartShape2DTests
    ShapeGeometryTests.cpp
    ShapeCollisionTests.cpp
    ../Native/SmartShape2D/ShapeCollision.cpp
    ../Native/SmartShape2D/ShapeGeometry.cpp)
target_compile_features(SmartShape2DTests PRIVATE cxx_std_20)
target_include_directories(SmartShape2DTests PRIVATE ../Native)
target_link_libraries(SmartShape2DTests PRIVATE Engine GTest::gtest_main)
ge_set_output_tests(SmartShape2DTests)
ge_stage_runtime_dependencies(SmartShape2DTests)
add_test(NAME SmartShape2DTests COMMAND SmartShape2DTests)
