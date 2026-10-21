cmake_minimum_required(VERSION 3.20)

project(Image LANGUAGES CXX)

# Image module: the engine's one CPU image decode (8-bit, 16-bit and float, with a
# size budget), header read, resample (RGBA8, plus the float RGBA resample of the HDR
# mip chain) and PNG encode. It compiles the stb_image, stb_image_resize2 and
# stb_image_write implementations once; nothing else in the tree calls them.
add_library(Image OBJECT
    Source/Decode.cpp
    Source/Encode.cpp
    Source/ImageHeader.cpp
    Source/PngDeclaration.cpp
    Source/Resample.cpp
    Source/StbImplementation.cpp
)

target_compile_features(Image PUBLIC cxx_std_20)

target_include_directories(Image
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_link_libraries(Image PUBLIC Logger)

find_package(stb CONFIG QUIET)
if(TARGET stb::stb)
    target_link_libraries(Image PRIVATE stb::stb)
else()
    find_path(GE_IMAGE_STB_INCLUDE_DIR "stb_image.h" REQUIRED)
    # SYSTEM: stb's own headers are not held to the module's -Wpedantic -Werror.
    target_include_directories(Image SYSTEM PRIVATE ${GE_IMAGE_STB_INCLUDE_DIR})
endif()

if(MSVC)
    target_compile_options(Image PRIVATE /W4)
else()
    target_compile_options(Image PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Self-find GTest (UI/Input/Spline pattern): gating on root-scope GTest state
# instead would silently drop the test target whenever the root skips its find.
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(ImageTests
        Tests/DecodeTests.cpp
        Tests/EncodeTests.cpp
        Tests/ImageHeaderTests.cpp
        Tests/ResampleTests.cpp
    )
    target_link_libraries(ImageTests PRIVATE Image Logger Types GTest::gtest GTest::gtest_main)
    set_target_properties(ImageTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests/Image"
    )
    ge_set_output_tests(ImageTests)
    ge_stage_runtime_dependencies(ImageTests)
    add_test(NAME ImageTests COMMAND ImageTests)
endif()
