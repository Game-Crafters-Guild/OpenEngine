# Types module tests (FlatMap, StringId, etc.)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTEST gtest_main)
    endif()
    if(NOT GTEST_FOUND)
        message(STATUS "GTest not found, skipping Types tests")
        return()
    endif()
endif()

add_executable(TypesTests
    ByteBufferTests.cpp
    ColorTests.cpp
    DisjointSetsTests.cpp
    FalseSharingTests.cpp
    FlatMapTests.cpp
    FlatSetTests.cpp
    FormatNumberTests.cpp
    Fnv1aTests.cpp
    FunctionRefTests.cpp
    InlineVectorTests.cpp
    NearestNameTests.cpp
    ParseIntegerTests.cpp
    ParseStrictDecimalTests.cpp
    PathUtilsTests.cpp
    StringUtilsTests.cpp
)

target_link_libraries(TypesTests
    PRIVATE
        Types
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(TypesTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

ge_set_output_tests(TypesTests)
ge_stage_runtime_dependencies(TypesTests)

add_test(NAME TypesTests COMMAND TypesTests)
set_tests_properties(TypesTests PROPERTIES
    TIMEOUT 30
    LABELS "Types;Unit"
)

# Only ctest builds these compile checks. Under clang, FunctionRefBoundToTemporary must fail (-Werror=dangling through
# GE_LIFETIMEBOUND); other compilers get no attribute and skip it. CompilePassControl must compile on every compiler.
set(TYPES_COMPILE_CHECKS CompilePassControl)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND TYPES_COMPILE_CHECKS FunctionRefBoundToTemporary)
endif()

foreach(check IN LISTS TYPES_COMPILE_CHECKS)
    add_library(TypesCompileFail_${check} OBJECT CompileFail/${check}.cpp)
    set_target_properties(TypesCompileFail_${check} PROPERTIES
        EXCLUDE_FROM_ALL TRUE
        EXCLUDE_FROM_DEFAULT_BUILD TRUE
        UNITY_BUILD OFF)
    target_link_libraries(TypesCompileFail_${check} PRIVATE Types)
    target_compile_options(TypesCompileFail_${check} PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Werror=dangling>)
    add_test(NAME TypesCompileFail.${check}
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                     --target TypesCompileFail_${check} --config $<CONFIG>)
    set_tests_properties(TypesCompileFail.${check} PROPERTIES
        LABELS "CompileFail"
        RUN_SERIAL TRUE)
endforeach()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_tests_properties(TypesCompileFail.FunctionRefBoundToTemporary PROPERTIES WILL_FAIL TRUE)
endif()
