# The Memory module's own suites. Both receive the allocation hook and the Testing headers
# as test executables (cmake/AllocationHook.cmake).

find_package(GTest REQUIRED)

# The counter and its windows, the stats registry, the arena, the object pool, the range allocator
# and scratch scopes,
# over the Memory module's own copy of each. JobSystem gives the Process window a real worker to
# fork onto, and scratch scopes the pool's ways of nesting bodies on one thread.
add_executable(MemoryTests
    AllocationCountScopeTests.cpp
    AllocationSiteTagTests.cpp
    ArenaImagePinTests.cpp
    ArenaTests.cpp
    ArenaUseTests.cpp
    ObjectPoolTests.cpp
    ObjectPoolUseTests.cpp
    RangeAllocatorTests.cpp
    ScopedAllocationFaultTests.cpp
    ScratchScopeTests.cpp
    TrackedBytesTests.cpp
)
target_compile_features(MemoryTests PRIVATE cxx_std_20)
target_link_libraries(MemoryTests PRIVATE Memory JobSystem Logger GTest::gtest GTest::gtest_main)
ge_set_output_tests(MemoryTests)
ge_stage_runtime_dependencies(MemoryTests)
add_test(NAME MemoryTests COMMAND $<TARGET_FILE:MemoryTests>)

# Engine's allocations, counted from a test executable. Registered only where they are counted:
# on Windows Engine.dll carries the hook only where GE_DEBUG_INSTRUMENTATION is 1, so a Release
# run would read a false zero; on macOS and Linux the replacements coalesce in every
# configuration.
add_executable(EngineAllocationHookTests EngineAllocationHookTests.cpp)
target_compile_features(EngineAllocationHookTests PRIVATE cxx_std_20)
target_link_libraries(EngineAllocationHookTests PRIVATE Engine GTest::gtest GTest::gtest_main)
ge_set_output_tests(EngineAllocationHookTests)
ge_stage_runtime_dependencies(EngineAllocationHookTests)
if(WIN32)
    add_test(NAME EngineAllocationHookTests CONFIGURATIONS Debug DebugFast
        COMMAND $<TARGET_FILE:EngineAllocationHookTests>)
else()
    add_test(NAME EngineAllocationHookTests COMMAND $<TARGET_FILE:EngineAllocationHookTests>)
endif()

# cmake/EngineModuleDuplicationCheck.cmake, run by configuring a scratch project with this
# build's generator and compiler: an executable linking Engine that also links a module Engine
# contains, splices its objects, or links an INTERFACE library that splices them is refused
# with the fix, and one linking Engine alone configures.
set(_ge_probe_source "${CMAKE_CURRENT_SOURCE_DIR}/EngineModuleDuplicationProbe")
set(_ge_probe_arguments
    -S "${_ge_probe_source}"
    -G "${CMAKE_GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DGE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND _ge_probe_arguments -A "${CMAKE_GENERATOR_PLATFORM}")
endif()
if(CMAKE_MAKE_PROGRAM)
    list(APPEND _ge_probe_arguments "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
endif()
add_test(NAME EngineModuleDuplicationCheck.RefusesAnExecutableLinkingEngineAndUI
    COMMAND ${CMAKE_COMMAND} ${_ge_probe_arguments}
        -B "${CMAKE_CURRENT_BINARY_DIR}/EngineModuleDuplicationProbe-UIBesideEngine"
        -DPROBE_CARRY_UI=Link)
set_tests_properties(EngineModuleDuplicationCheck.RefusesAnExecutableLinkingEngineAndUI PROPERTIES
    PASS_REGULAR_EXPRESSION "Probe links Engine and UI.*Link Engine alone")
add_test(NAME EngineModuleDuplicationCheck.RefusesAnExecutableSplicingUIObjects
    COMMAND ${CMAKE_COMMAND} ${_ge_probe_arguments}
        -B "${CMAKE_CURRENT_BINARY_DIR}/EngineModuleDuplicationProbe-UISplicedBesideEngine"
        -DPROBE_CARRY_UI=Splice)
set_tests_properties(EngineModuleDuplicationCheck.RefusesAnExecutableSplicingUIObjects PROPERTIES
    PASS_REGULAR_EXPRESSION "Probe links Engine and UI.*Link Engine alone")
add_test(NAME EngineModuleDuplicationCheck.RefusesAnInterfaceLibraryCarryingUIObjects
    COMMAND ${CMAKE_COMMAND} ${_ge_probe_arguments}
        -B "${CMAKE_CURRENT_BINARY_DIR}/EngineModuleDuplicationProbe-UIObjectsBesideEngine"
        -DPROBE_CARRY_UI=InterfaceSplice)
set_tests_properties(EngineModuleDuplicationCheck.RefusesAnInterfaceLibraryCarryingUIObjects PROPERTIES
    PASS_REGULAR_EXPRESSION "Probe links Engine and UI.*Link Engine alone")
add_test(NAME EngineModuleDuplicationCheck.AcceptsAnExecutableLinkingEngineAlone
    COMMAND ${CMAKE_COMMAND} ${_ge_probe_arguments}
        -B "${CMAKE_CURRENT_BINARY_DIR}/EngineModuleDuplicationProbe-EngineAlone"
        -DPROBE_CARRY_UI=None)
