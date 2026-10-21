# EnginePublicInterface.cmake
#
# The compile definitions and include directories Engine and its modules hand to
# every target that links them. Engine re-exports each module's interface
# (Engine/CMakeLists.txt), so an entry here is on the command line of the Editor,
# the Player, every test and every package module: adding one, removing one or
# changing a definition's value recompiles all of them. Each entry is therefore a
# reviewed decision, listed below with the reason a consumer needs it, and
# configure fails on an interface entry that is not listed.
#
# What needs no entry: a module's own Include/ directory, the install-tree
# spelling of an include directory, and Engine's re-export of a checked module's
# interface (that module's entries are checked where they are declared).
#
# A definition only the module's own sources read is PRIVATE. One a consumer reads
# (a public header, an application source, a test) is PUBLIC and listed here.

# The reviewed lists, set in the caller's scope. A function rather than plain variables
# because the check runs deferred, in the top-level directory's scope.
function(_ge_engine_public_interface_lists)
    # <target>|<definition name>|<who reads it>
    set(_GE_PUBLIC_DEFINITIONS
        "Engine|GE_ENABLE_SCRIPTING|Editor and Player sources, NativeScripting tests"
        "Engine|GE_ENABLE_CPU_PROFILING|Engine/Include profiling headers, Editor sources"
        "Engine|GE_ENABLE_GPU_PROFILING|Editor sources"
        "Engine|GE_ENABLE_METRICS|Engine/Include metrics header, Editor sources"
        "Engine|GE_HAVE_KTX|Engine/Include texture header, Engine tests"
        "Logger|LOGGER_ENABLE_FILE_LOGGING|Logger public header, Editor and Player sources"
        "Logger|LOGGER_ENABLE_TIMESTAMPS|LoggerAsyncTests (FileSinkFormatTests.cpp)"
        "Logger|LOGGER_ENABLE_SOURCE_LOCATION|Logger public header, Editor sources"
        "ECS|ECS_SIMD_ENABLED|ECS public header"
        "ECS|ECS_SIMD_X64|ECS public header"
        "ECS|ECS_SIMD_ARM64|ECS public header"
        "GameEngineRendering|GE_ENABLE_GPU_PROFILING|Rendering sources (the same switch Engine exports)"
        "GameEngineRendering|RENDERING_ENABLE_SPIRV_REFLECTION|Source/Vulkan/VulkanDevice.h, which Rendering tests include; the reflection tests"
        "GameEngineRendering|GE_ENABLE_DEBUG_BARRIERS|Source/Vulkan/VulkanDevice.h, which Rendering tests include"
        "GameEngineRendering|RENDERING_HAS_VULKAN|RenderGraphTests, CpuTextureSourceTests"
        "GameEngineRendering|RENDERING_HAS_SHADERC|MetalBackendHeadlessTests"
        "UI|GE_HAVE_YOGA|UI public headers, UI tests"
        "UI|GE_HAVE_FREETYPE|Text public headers (UI re-exports Text's switches)"
        "UI|GE_HAVE_HARFBUZZ|Text public headers (UI re-exports Text's switches)"
        "Text|GE_HAVE_FREETYPE|Text public headers, Text and Rendering tests"
        "Text|GE_HAVE_HARFBUZZ|Text public headers, wasm tests"
        "Pathfinding|GE_PATHFINDING_BACKEND_DETOUR|PathfindingECSTests (NavigationSolveBudgetTests.cpp)"
    )

    # <target>|<include directory>|<reason>; <src> is the source tree, <build> the build
    # tree, <vcpkg> the vcpkg install tree of the target triplet.
    set(_GE_PUBLIC_INCLUDE_EXCEPTIONS
        "Engine|<build>/generated|generated headers (EngineVersion.h, ...); #3695"
        "Engine|<src>/Engine/Modules/VolumetricFog/Include|#3695"
        "Engine|<src>/Engine/Modules/HeightFog/Include|#3695"
        "Engine|<src>/Engine/Modules/Particles/Include|#3695"
        "Engine|<src>/Engine/Modules/Ocean/Include|#3695"
        "Engine|<src>/Engine/ECSModules/Rendering/Include|#3695"
        "Engine|<src>/Engine/ECSModules/Audio/Include|#3695"
        "GameEngineRendering|<src>/Engine/Modules/Rendering/Source/Vulkan|#3695"
        "GameEngineRendering|<vcpkg>/include|#3695"
        "FileWatcher|<src>/Engine/Include|#3695"
        "AssetRuntime|<src>/Engine/Include|#3695"
        "AssetRuntime|<src>/Engine/Modules/Animation/Include|#3695"
        "Scripting|<src>/Engine/Include|#3695"
        "Platform|<src>/Engine/Include|#3695"
        "RenderServices|<src>/Engine/Include|#3695"
        "RenderServices|<src>/Engine/Modules/VolumetricFog/Include|#3695"
        "RenderServices|<src>/Engine/ECSModules/Rendering/Include|#3695"
        "Graph|<src>/Engine/Modules/Rendering/Include|#3695"
        "Particles|<src>/Engine/Modules/Rendering/Include|Components/Rendering/ParticleRenderer.h (Rendering/RenderLayerMask.h)"
        "PathfindingECS|$<TARGET_PROPERTY:FileWatcher,INTERFACE_INCLUDE_DIRECTORIES>|#3695"
    )
    set(_GE_PUBLIC_DEFINITIONS "${_GE_PUBLIC_DEFINITIONS}" PARENT_SCOPE)
    set(_GE_PUBLIC_INCLUDE_EXCEPTIONS "${_GE_PUBLIC_INCLUDE_EXCEPTIONS}" PARENT_SCOPE)
endfunction()

# The definition name an interface entry declares: "NAME", "NAME=value" and
# "$<condition:NAME=value>" all name NAME.
function(_ge_interface_definition_name entry outVar)
    set(_rest "${entry}")
    while(_rest MATCHES "^\\$<")
        string(REGEX REPLACE "^\\$<(\\$<[^>]*>|[^:$])*:" "" _stripped "${_rest}")
        if(_stripped STREQUAL _rest)
            break()
        endif()
        set(_rest "${_stripped}")
    endwhile()
    set(_name "")
    if(_rest MATCHES "^([A-Za-z_][A-Za-z0-9_]*)")
        set(_name "${CMAKE_MATCH_1}")
    endif()
    set(${outVar} "${_name}" PARENT_SCOPE)
endfunction()

# An include directory spelled with the <src>, <build> and <vcpkg> placeholders.
function(_ge_interface_include_spelling entry outVar)
    set(_spelling "${entry}")
    if(_spelling MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
        set(_spelling "${CMAKE_MATCH_1}")
    endif()
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        string(REPLACE "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}" "<vcpkg>" _spelling "${_spelling}")
    endif()
    string(REPLACE "${CMAKE_BINARY_DIR}" "<build>" _spelling "${_spelling}")
    string(REPLACE "${CMAKE_SOURCE_DIR}" "<src>" _spelling "${_spelling}")
    set(${outVar} "${_spelling}" PARENT_SCOPE)
endfunction()

# ge_check_engine_public_interface(<target>...): fails configure on an interface
# definition or include directory of one of <target>... that the lists above do
# not name. Every unlisted entry is reported at once.
function(ge_check_engine_public_interface)
    set(_checked ${ARGN})
    _ge_engine_public_interface_lists()
    set(_ge_listed_definitions "")
    foreach(_listed IN LISTS _GE_PUBLIC_DEFINITIONS)
        string(REGEX MATCH "^[^|]+[|][^|]+[|]" _key "${_listed}")
        list(APPEND _ge_listed_definitions "${_key}")
    endforeach()
    set(_ge_listed_includes "")
    foreach(_listed IN LISTS _GE_PUBLIC_INCLUDE_EXCEPTIONS)
        string(REGEX MATCH "^[^|]+[|][^|]+[|]" _key "${_listed}")
        list(APPEND _ge_listed_includes "${_key}")
    endforeach()
    set(_problems "")
    foreach(_target IN LISTS _checked)
        if(NOT TARGET ${_target})
            continue()
        endif()
        get_target_property(_definitions ${_target} INTERFACE_COMPILE_DEFINITIONS)
        if(_definitions)
            foreach(_entry IN LISTS _definitions)
                if(_entry MATCHES "^\\$<TARGET_PROPERTY:([^,>]+),INTERFACE_COMPILE_DEFINITIONS>$"
                   AND CMAKE_MATCH_1 IN_LIST _checked)
                    continue()
                endif()
                _ge_interface_definition_name("${_entry}" _name)
                if(NOT "${_target}|${_name}|" IN_LIST _ge_listed_definitions)
                    list(APPEND _problems "definition ${_name} on ${_target} (${_entry})")
                endif()
            endforeach()
        endif()

        get_target_property(_includes ${_target} INTERFACE_INCLUDE_DIRECTORIES)
        get_target_property(_sourceDir ${_target} SOURCE_DIR)
        _ge_interface_include_spelling("${_sourceDir}/Include" _ownInclude)
        if(_includes)
            foreach(_entry IN LISTS _includes)
                if(_entry MATCHES "^\\$<INSTALL_INTERFACE:")
                    continue()
                endif()
                if(_target STREQUAL "Engine"
                   AND _entry MATCHES "^\\$<TARGET_PROPERTY:([^,>]+),INTERFACE_INCLUDE_DIRECTORIES>$"
                   AND CMAKE_MATCH_1 IN_LIST _checked)
                    continue()
                endif()
                _ge_interface_include_spelling("${_entry}" _spelling)
                if(_spelling STREQUAL _ownInclude)
                    continue()
                endif()
                if(NOT "${_target}|${_spelling}|" IN_LIST _ge_listed_includes)
                    list(APPEND _problems "include directory ${_spelling} on ${_target}")
                endif()
            endforeach()
        endif()
    endforeach()

    if(_problems)
        list(JOIN _problems "\n    " _problemText)
        message(FATAL_ERROR
            "Engine's public interface gained entries nobody reviewed:\n    ${_problemText}\n"
            "  Each one is on the command line of every target that links Engine, so adding it, "
            "removing it or changing its value recompiles all of them. Make it PRIVATE if only "
            "the module's own sources read it; otherwise add it to the lists in "
            "cmake/EnginePublicInterface.cmake with who reads it, in the same pull request.")
    endif()
endfunction()
