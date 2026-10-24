# ComponentScanner — the build-time generator of the engine's component reflection
# (ComponentReflection.gen.cpp) and the web library's component types (components.d.ts,
# components.json), the script API description (Engine/Scripting/engine-api/), and the
# tool the editor runs over native user scripts (see ComponentScanner.h). Standard library
# only, plus the engine's header-only Types/Fnv1a.h and Types/ParseNumber.h: the Engine build
# depends on its output, and a cross build (the wasm presets) runs it on the build host.
#
# Added from Engine/CMakeLists.txt; also configurable on its own (a host-tool build for a
# cross-compiling tree, or a Linux check without the engine's dependencies).
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ComponentScanner LANGUAGES CXX)
    option(BUILD_TESTING "Build the ComponentScanner tests" OFF)
endif()

add_library(ComponentScannerCore STATIC
    ApiDraftReader.cpp
    ApiJsonEmitter.cpp
    ApiKindResolver.cpp
    ApiMarkerLines.cpp
    ApiModel.cpp
    ApiResolver.cpp
    ApiScanner.cpp
    BindingRules.cpp
    CommentStripper.cpp
    ComponentMirror.cpp
    ComponentMirrorCSharpEmitter.cpp
    ComponentMirrorLayoutEmitter.cpp
    ComponentMirrorResolver.cpp
    ComponentScanner.cpp
    CSharpBindingEmitter.cpp
    DtsEmitter.cpp
    EnumScanner.cpp
    FieldKindResolver.cpp
    FunctionParser.cpp
    JsonEmitter.cpp
    Lexical.cpp
    MarkerScanner.cpp
    NativeBindingEmitter.cpp
    NumberFormat.cpp
    ReflectedModel.cpp
    ReflectionEmitter.cpp
    SourceFiles.cpp
    StructParser.cpp
    TypeAliasScanner.cpp
    WebComponentModel.cpp)
target_compile_features(ComponentScannerCore PUBLIC cxx_std_20)
target_include_directories(ComponentScannerCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Types/Fnv1a.h, the engine's one FNV-1a (header-only, standard library only): the script API
# description's module hash. Types/ParseNumber.h (header-only, standard and C library only):
# the locale-independent conversion of floating-point literals.
target_include_directories(ComponentScannerCore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Modules/Types/Include)
set_target_properties(ComponentScannerCore PROPERTIES FOLDER "Tools")

add_executable(ComponentScanner main.cpp)
target_link_libraries(ComponentScanner PRIVATE ComponentScannerCore)
set_target_properties(ComponentScanner PROPERTIES FOLDER "Tools")
if(EMSCRIPTEN)
    # The wasm presets run the scanner under node (CMAKE_CROSSCOMPILING_EMULATOR): it reads
    # the headers and writes the generated files straight on the host file system, and exits
    # with its real exit code.
    target_link_options(ComponentScanner PRIVATE "SHELL:-s NODERAWFS=1" "SHELL:-s EXIT_RUNTIME=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1")
endif()
if(COMMAND ge_set_output_tools)
    # Output layout: bin/<Config>/Tools, beside ShaderReflect and MaterialVariantCook.
    ge_set_output_tools(ComponentScanner)
endif()

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(ComponentScannerTests
        Tests/ApiDescriptionDriftTests.cpp
        Tests/BaseDetectionTests.cpp
        Tests/BindingEmitterTests.cpp
        Tests/CommentStripperTests.cpp
        Tests/ComponentMirrorTests.cpp
        Tests/EnumScannerTests.cpp
        Tests/HostileHeaderTests.cpp
        Tests/MarkerAttachmentTests.cpp
        Tests/MarkerScannerTests.cpp
        Tests/NumberFormatTests.cpp
        Tests/ReflectionEmitterTests.cpp
        Tests/ScanModeTests.cpp
        Tests/ScriptApiTests.cpp
        Tests/StructParserTests.cpp
        Tests/WebEmitterTests.cpp)
    target_link_libraries(ComponentScannerTests PRIVATE ComponentScannerCore GTest::gtest GTest::gtest_main)
    # Tests/TestTempDir.h for the scratch trees the end-to-end cases scan.
    target_include_directories(ComponentScannerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../Tests)
    # Dev-only source anchoring: the fixture trees the scan-mode cases run over, the engine's
    # FieldTypeId declaration the kind table is checked against, and the engine's header trees
    # and committed script API description and C# binding the drift test compares.
    target_compile_definitions(ComponentScannerTests PRIVATE
        GE_COMPONENT_SCANNER_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures"
        GE_ECS_REFLECTION_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Modules/ECS/Include/ECS/Reflection.h"
        GE_ENGINE_MODULES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Modules"
        GE_ENGINE_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Include"
        GE_ENGINE_ECS_MODULES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/ECSModules"
        GE_ENGINE_API_DESCRIPTION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Scripting/engine-api"
        GE_CSHARP_BINDINGS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../Managed/Scripting.Bindings/Generated")
    set_target_properties(ComponentScannerTests PROPERTIES FOLDER "Tools/Tests")
    if(COMMAND ge_set_output_tests)
        ge_set_output_tests(ComponentScannerTests)
        ge_stage_runtime_dependencies(ComponentScannerTests)
    endif()
    add_test(NAME ComponentScannerTests COMMAND ComponentScannerTests)
endif()
