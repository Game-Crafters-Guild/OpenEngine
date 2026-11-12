find_package(GTest REQUIRED)

add_executable(SerializationTests
    JsonFieldReaderTests.cpp
)
target_compile_features(SerializationTests PUBLIC cxx_std_20)
target_link_libraries(SerializationTests PRIVATE GTest::gtest GTest::gtest_main Serialization)
ge_set_output_tests(SerializationTests)
ge_stage_runtime_dependencies(SerializationTests)

add_test(NAME SerializationTests COMMAND SerializationTests)
set_tests_properties(SerializationTests PROPERTIES TIMEOUT 60 LABELS "Serialization;Unit")
