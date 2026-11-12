cmake_minimum_required(VERSION 3.20)
project(Input LANGUAGES CXX)

# Public headers and sources
file(GLOB_RECURSE INPUT_PUBLIC_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/Include/Input/*.h)
set(INPUT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/InputScriptApi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/InputSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Gamepad/GamepadPoller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Gamepad/GamepadEdgeTracker.cpp
)

# One gamepad backend per platform (Source/Gamepad/): GameInput on Windows, Apple's Game Controller
# framework on macOS, GLFW's joystick API on Linux and in the browser (Emscripten's GLFW).
if(WIN32)
    list(APPEND INPUT_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/Gamepad/GamepadBackend_GameInput.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/Gamepad/GameInputReading.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/Gamepad/ControllerMapping/ControllerMapping.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/Gamepad/ControllerMapping/ControllerMappingDatabase.cpp)

    # GameInput's controller kind reads a non-Xbox pad as raw controls; the community mapping
    # database (Data/gamecontrollerdb.txt) maps them. The build compiles the file into the module,
    # so every executable and every exported game carries it with no staged file.
    set(INPUT_CONTROLLER_MAPPINGS_TXT ${CMAKE_CURRENT_SOURCE_DIR}/Data/gamecontrollerdb.txt)
    set(INPUT_CONTROLLER_MAPPINGS_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/ControllerMappingText.h)
    add_custom_command(
        OUTPUT ${INPUT_CONTROLLER_MAPPINGS_HEADER}
        COMMAND ${CMAKE_COMMAND} -DSRC=${INPUT_CONTROLLER_MAPPINGS_TXT}
                                 -DOUT=${INPUT_CONTROLLER_MAPPINGS_HEADER}
                                 -DSYMBOL=kControllerMappingText
                                 -P ${CMAKE_SOURCE_DIR}/cmake/EmbedTextFile.cmake
        DEPENDS ${INPUT_CONTROLLER_MAPPINGS_TXT} ${CMAKE_SOURCE_DIR}/cmake/EmbedTextFile.cmake
        COMMENT "Embedding the controller mapping database"
        VERBATIM)
    list(APPEND INPUT_SOURCES ${INPUT_CONTROLLER_MAPPINGS_HEADER})
elseif(APPLE)
    # The framework's extended gamepad profile is the standard layout: no mapping database.
    list(APPEND INPUT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/Source/Gamepad/GamepadBackend_GameController.mm)
else()
    list(APPEND INPUT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/Source/Gamepad/GamepadBackend_Glfw.cpp)
endif()

add_library(Input OBJECT ${INPUT_PUBLIC_HEADERS} ${INPUT_SOURCES})

# Public includes
target_include_directories(Input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include)

# Link to core engine modules used by InputSystem
# Types: basic math/types; Mathematics: Vector2 in the public API;
# Platform: window/input events
if(TARGET Types)
    target_link_libraries(Input PUBLIC Types)
endif()
if(TARGET Mathematics)
    target_link_libraries(Input PUBLIC Mathematics)
endif()
if(TARGET Platform)
    target_link_libraries(Input PUBLIC Platform)
endif()
# JobSystem: the poller starts its backend on a blocking channel; Logger: the backend's start line.
target_link_libraries(Input PRIVATE JobSystem Logger)
target_include_directories(Input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
if(WIN32)
    # GameInput.lib loads the GameInput runtime when GameInputCreate first runs; a PC without the
    # runtime fails that call and gamepads stay off (GamepadRuntimeState::RuntimeMissing).
    find_package(gameinput CONFIG REQUIRED)
    target_link_libraries(Input PRIVATE Microsoft::GameInput)
    target_include_directories(Input PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
elseif(APPLE)
    # GameController.framework is a system framework, as the Platform module's frameworks.
    target_link_libraries(Input PRIVATE "-framework GameController" "-framework Foundation")
    set_target_properties(Input PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
elseif(NOT EMSCRIPTEN)
    find_package(glfw3 CONFIG REQUIRED)
    target_link_libraries(Input PRIVATE glfw)
endif()

set_target_properties(Input PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Input"
)

# Unit tests for Input module (optional)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(InputSystemTests
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/GamepadPollerTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/InputScriptApiTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/InputSystemTests.cpp
    )
    # The mapping database is built only with the Windows backend. The GameInput reading's tests
    # build its state struct, so they take GameInput's header.
    if(WIN32)
        target_sources(InputSystemTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ControllerMappingDatabaseTests.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/GamepadBackendGameInputTests.cpp)
        target_include_directories(InputSystemTests PRIVATE
            $<TARGET_PROPERTY:Microsoft::GameInput,INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
    # The macOS backend's tests post the framework's snapshot controllers; no pad is needed.
    if(APPLE)
        target_sources(InputSystemTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Tests/GamepadBackendGameControllerTests.mm)
        target_link_libraries(InputSystemTests PRIVATE "-framework GameController" "-framework Foundation")
        set_target_properties(InputSystemTests PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
    endif()

    # Modules are OBJECT libs aggregated into Engine.dll; link Engine to
    # resolve module symbols via the import lib. See GenerationalVector/Tests
    # for the same fix pattern.
    find_package(nlohmann_json CONFIG REQUIRED)
    target_link_libraries(InputSystemTests PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
        nlohmann_json::nlohmann_json
    )
    # Source: GamepadPollerTests drives the header-only slot table and snapshot buffer the Windows
    # backend uses; JobSystem: the poller's pool.
    target_include_directories(InputSystemTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        $<TARGET_PROPERTY:JobSystem,INTERFACE_INCLUDE_DIRECTORIES>)
    # Dev-only source anchoring: the module's committed script API description, whose record
    # layouts InputScriptApiTests checks against the compiler's.
    target_compile_definitions(InputSystemTests PRIVATE
        GE_ENGINE_API_DESCRIPTION="${CMAKE_CURRENT_SOURCE_DIR}/../../Scripting/engine-api/Input.json")

    set_target_properties(InputSystemTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Input/Tests"
    )

    ge_set_output_tests(InputSystemTests)
    ge_stage_runtime_dependencies(InputSystemTests)

    add_test(NAME InputSystemTests COMMAND InputSystemTests)
endif()

