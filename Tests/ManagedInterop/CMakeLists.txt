# DebugFast exercises Debug-only managed test hooks. Build, stage and run the same configuration.
set(managedConfig "$<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:DebugFast>>,Debug,Release>")
set(testProject "${CMAKE_SOURCE_DIR}/Managed/InteropTests/InteropTests.csproj")
set(testOutput "${CMAKE_SOURCE_DIR}/Managed/InteropTests/bin/${managedConfig}/net10.0")

add_custom_target(BuildManagedInteropTests ALL
    COMMAND ${CMAKE_COMMAND} -E env --unset=TargetName --unset=Platform
        "${DOTNET_EXECUTABLE}" build "${testProject}" -c "${managedConfig}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:GameEngine.NativeShim> "${testOutput}/$<TARGET_FILE_NAME:GameEngine.NativeShim>"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${testOutput}/hostfxr.dll" "${testOutput}/nethost.dll"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Building managed interop tests and staging their native test double"
    VERBATIM)
add_dependencies(BuildManagedInteropTests GameEngine.NativeShim)

# ProjectReferences share these outputs with application builds. Finish their
# CMake producers before the test project's MSBuild graph visits them again.
foreach(assembly IN ITEMS HotReloadAssembly CoreBridgeAssembly ScriptingBindingsAssembly
        EcsAbiAssembly ScriptingAbiAssembly ScriptingRuntimeAssembly EditorManagedAssembly)
    if(TARGET ${assembly})
        add_dependencies(BuildManagedInteropTests ${assembly})
    endif()
endforeach()

add_test(NAME ManagedInteropTests
    COMMAND "${DOTNET_EXECUTABLE}" test "${testProject}" -c "${managedConfig}" --no-build --no-restore)
set_tests_properties(ManagedInteropTests PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    ENVIRONMENT "DOTNET_ROLL_FORWARD=LatestPatch;GE_NATIVE_DIR=$<TARGET_FILE_DIR:GameEngine.NativeShim>")
