# Fails when a shader source the Player stages includes a file the Player does not stage.
# The runtime compiler (ShaderCompileService) substitutes empty text for an include it cannot
# find, so a missing file shows up only as a variant that does not compile at run time.
#
# An include resolves against the including file's directory, then against the staged
# include roots the runtime compile paths search: Graph/Nodes, GPUFogParticles, the
# Assets/Shaders root, Adapters and Includes for materials, and CBT and TerrainGrass for the
# grass compute kernels (GrassComputePrograms.cpp). Every staged source is scanned, so the check
# covers the include closure of every shader the Player ships, inactive #if branches included.
if(NOT DEFINED PLAYER_CONTENT_DIR)
    message(FATAL_ERROR "PLAYER_CONTENT_DIR must identify the Player's staged content directory")
endif()

set(shaders_root "${PLAYER_CONTENT_DIR}/Assets/Shaders")
set(include_roots
    "${shaders_root}/Graph/Nodes"
    "${shaders_root}/GPUFogParticles"
    "${shaders_root}"
    "${shaders_root}/Adapters"
    "${shaders_root}/Includes"
    "${shaders_root}/CBT"
    "${shaders_root}/TerrainGrass")

file(GLOB_RECURSE sources LIST_DIRECTORIES false
    "${shaders_root}/*.glsl" "${shaders_root}/*.vert" "${shaders_root}/*.frag"
    "${shaders_root}/*.comp" "${shaders_root}/*.geom")
if(NOT sources)
    message(FATAL_ERROR "No staged shader sources under ${shaders_root}")
endif()

set(missing "")
foreach(source IN LISTS sources)
    file(STRINGS "${source}" include_lines REGEX "^[ \t]*#[ \t]*include[ \t]*\"[^\"]+\"")
    get_filename_component(source_dir "${source}" DIRECTORY)
    foreach(line IN LISTS include_lines)
        string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\".*$" "\\1" requested "${line}")
        set(found FALSE)
        foreach(dir IN ITEMS "${source_dir}" ${include_roots})
            if(EXISTS "${dir}/${requested}")
                set(found TRUE)
                break()
            endif()
        endforeach()
        if(NOT found)
            file(RELATIVE_PATH relative_source "${shaders_root}" "${source}")
            string(APPEND missing "\n  ${relative_source} includes \"${requested}\"")
        endif()
    endforeach()
endforeach()

if(missing)
    message(FATAL_ERROR
        "The Player stages shaders whose includes it does not stage (Apps/Player/CMakeLists.txt):${missing}")
endif()
