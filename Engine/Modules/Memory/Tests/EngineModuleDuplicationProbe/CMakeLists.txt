# A scratch project for cmake/EngineModuleDuplicationCheck.cmake: an Engine shared library
# that contains a UI OBJECT module, and an executable that links Engine and carries UI's
# objects as PROBE_CARRY_UI says: None, Link (UI linked beside Engine), Splice
# ($<TARGET_OBJECTS:UI> in its own sources) or InterfaceSplice (an INTERFACE library that
# carries $<TARGET_OBJECTS:UI>, linked beside Engine). The check refuses all but None.
# Configured by the EngineModuleDuplicationCheck tests in ../CMakeLists.txt; nothing is built.

cmake_minimum_required(VERSION 3.25)
project(EngineModuleDuplicationProbe LANGUAGES CXX)

set(PROBE_CARRY_UI "None" CACHE STRING "How Probe carries UI's objects beside Engine: None, Link, Splice or InterfaceSplice")

include(${GE_SOURCE_DIR}/cmake/EngineModuleDuplicationCheck.cmake)

add_library(UI OBJECT Module.cpp)
add_library(Engine SHARED Module.cpp)
target_link_libraries(Engine PRIVATE UI)

add_executable(Probe Main.cpp)
target_link_libraries(Probe PRIVATE Engine)
if(PROBE_CARRY_UI STREQUAL "Link")
    target_link_libraries(Probe PRIVATE UI)
elseif(PROBE_CARRY_UI STREQUAL "Splice")
    target_sources(Probe PRIVATE $<TARGET_OBJECTS:UI>)
elseif(PROBE_CARRY_UI STREQUAL "InterfaceSplice")
    add_library(UIObjects INTERFACE)
    target_sources(UIObjects INTERFACE $<TARGET_OBJECTS:UI>)
    target_link_libraries(Probe PRIVATE UIObjects)
elseif(NOT PROBE_CARRY_UI STREQUAL "None")
    message(FATAL_ERROR "PROBE_CARRY_UI is '${PROBE_CARRY_UI}'; use None, Link, Splice or InterfaceSplice.")
endif()
