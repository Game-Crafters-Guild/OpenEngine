# The web library's page reflection (Apps/WebLibrary/Source/PageReflection.cpp) on the desktop.
# The file reads only the ECS field tables, so it compiles here without Emscripten; the suite
# links Engine because its generated reflection unit is what fills those tables.

if(BUILD_TESTING AND (GTest_FOUND OR TARGET GTest::gtest))
    find_package(nlohmann_json CONFIG REQUIRED)
    add_executable(WebPageReflectionTests
        PageReflectionTests.cpp
        ${CMAKE_SOURCE_DIR}/Apps/WebLibrary/Source/PageReflection.cpp)
    target_include_directories(WebPageReflectionTests PRIVATE ${CMAKE_SOURCE_DIR}/Apps/WebLibrary/Source)
    target_compile_features(WebPageReflectionTests PRIVATE cxx_std_20)
    target_link_libraries(WebPageReflectionTests PRIVATE
        Engine nlohmann_json::nlohmann_json GTest::gtest GTest::gtest_main)
    ge_set_output_tests(WebPageReflectionTests)
    ge_stage_runtime_dependencies(WebPageReflectionTests)
    add_test(NAME WebPageReflectionTests COMMAND $<TARGET_FILE:WebPageReflectionTests>)
endif()
