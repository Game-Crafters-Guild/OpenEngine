cmake_minimum_required(VERSION 3.20)

project(TerrainGrass LANGUAGES CXX)

add_library(TerrainGrass OBJECT
    Include/TerrainGrass/GrassBladeRows.h
    Include/TerrainGrass/GrassComputePrograms.h
    Include/TerrainGrass/GrassMaterialKeywords.h
    Include/TerrainGrass/GrassPlacementElisionInputs.h
    Include/TerrainGrass/GrassPlacementModel.h
    Include/TerrainGrass/TerrainGrassClamps.h
    Include/TerrainGrass/TerrainGrassPresets.h
    Include/TerrainGrass/TerrainGrassPlacementStats.h
    Include/TerrainGrass/TerrainGrassRenderFeature.h
    Include/TerrainGrass/TerrainGrassRenderNode.h
    Include/TerrainGrass/RegisterTerrainGrass.h
    Include/TerrainGrass/Scene/TerrainGrassSceneSchemas.h
    Source/GrassComputePrograms.cpp
    Source/GrassPlacementElisionInputs.cpp
    Source/TerrainGrassComponents.cpp
    Source/TerrainGrassPlacementStats.cpp
    Source/TerrainGrassRenderFeature.cpp
    Source/TerrainGrassRenderNode.cpp
    Source/RegisterTerrainGrass.cpp
    Source/Scene/TerrainGrassSceneSchemas.cpp
)

target_compile_features(TerrainGrass PUBLIC cxx_std_20)

target_include_directories(TerrainGrass
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(TerrainGrass
    PUBLIC
        TerrainECS
        GameEngineRendering
        Logger
    PRIVATE
        Terrain
)

target_include_directories(TerrainGrass PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_INCLUDE_DIRECTORIES>
)

if(MSVC)
    target_compile_options(TerrainGrass PRIVATE /W4 /bigobj)
else()
    target_compile_options(TerrainGrass PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(BUILD_TESTING)
    # This module is added BEFORE the Rendering module and the root test block, so no
    # find_package(GTest) has run yet in any enclosing scope: without its own find here the
    # guard below is silently false and the suite never generates (the CBTTerrain test block
    # does the same normalize for the same reason).
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND AND TARGET GTest::gtest)
        set(GTest_FOUND TRUE)
    endif()
    if(GTest_FOUND OR TARGET GTest::gtest)
        # Compiles TerrainGrassPlacementStats.cpp DIRECTLY rather than linking the
        # TerrainGrass object library: the stats class deliberately depends on
        # nothing above Rendering, so the suite needs a device and not the render
        # feature's whole TerrainECS/RenderServices closure.
        # The ground splat SOURCE SELECTION is lifted verbatim out of the shipped shared include and
        # compiled into this suite as C++ (CBTTerrain/Tests/GlslShim.h), so the tests execute the
        # shader rather than a copy of it that can drift. A missing or reordered marker is a hard
        # error in the extraction step: extracting nothing would leave the suite green with no
        # shader in it. The extractor and the shim live in the CBTTerrain test tree, where this
        # pattern started; both are generic and this is their second consumer.
        set(GRASS_ATLAS_SPLAT_GLSL
            ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/TerrainGrass/grass_atlas_splat.glsl)
        set(GRASS_GLSL_SHIM_DIR ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Tests)
        set(GRASS_SPLAT_SOURCE_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassAtlasSplatSourceExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_SPLAT_SOURCE_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_ATLAS_SPLAT_GLSL}
                                     -DOUT=${GRASS_SPLAT_SOURCE_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_GRASS_SPLAT_SOURCE_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_GRASS_SPLAT_SOURCE_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_ATLAS_SPLAT_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the ground splat source selection from grass_atlas_splat.glsl"
            VERBATIM)

        # Same seam for the wind seed's noise-domain offset, so the suite EXECUTES the shipped
        # expression rather than restating it: a restatement could not tell whether seed 0 is still
        # the identity that the "unseeded content is unchanged" claim rests on.
        set(GRASS_VERTEX_MODIFIER_GLSL
            ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/TerrainGrass/terrain_grass_vertex_modifier.glsl)
        set(GRASS_WIND_SEED_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassWindSeedOffsetExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_WIND_SEED_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_VERTEX_MODIFIER_GLSL}
                                     -DOUT=${GRASS_WIND_SEED_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_WIND_SEED_OFFSET_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_WIND_SEED_OFFSET_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_VERTEX_MODIFIER_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the wind seed offset from terrain_grass_vertex_modifier.glsl"
            VERBATIM)

        # And for one blade vertex's shape and ground-colour sample point, so GrassBladeVertexTests
        # executes the shipped geometry: a sample point that followed the vertex up the blade drew
        # the ground's pattern up it (#4206).
        set(GRASS_BLADE_VERTEX_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassBladeVertexExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_BLADE_VERTEX_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_VERTEX_MODIFIER_GLSL}
                                     -DOUT=${GRASS_BLADE_VERTEX_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_GRASS_BLADE_VERTEX_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_GRASS_BLADE_VERTEX_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_VERTEX_MODIFIER_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the blade vertex from terrain_grass_vertex_modifier.glsl"
            VERBATIM)

        # And for the grass canopy's coverage and occlusion, which the ground and the blade root
        # share, so the contact's occlusion parity is pinned by the shipped expressions.
        set(GRASS_CANOPY_GLSL
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes/terrain_grass_canopy.glsl)
        set(GRASS_CANOPY_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassCanopyExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_CANOPY_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_CANOPY_GLSL}
                                     -DOUT=${GRASS_CANOPY_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_GRASS_CANOPY_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_GRASS_CANOPY_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_CANOPY_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the grass canopy from terrain_grass_canopy.glsl"
            VERBATIM)

        # And for the terrain colour map's apply, which the ground and the blade root share.
        set(TERRAIN_COLOR_MAP_GLSL
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes/terrain_color_map.glsl)
        set(TERRAIN_COLOR_MAP_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/TerrainColorMapExtracted.h)
        add_custom_command(
            OUTPUT ${TERRAIN_COLOR_MAP_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${TERRAIN_COLOR_MAP_GLSL}
                                     -DOUT=${TERRAIN_COLOR_MAP_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_TERRAIN_COLOR_MAP_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_TERRAIN_COLOR_MAP_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${TERRAIN_COLOR_MAP_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the colour map apply from terrain_color_map.glsl"
            VERBATIM)

        # And for the instance's packed blade shape and per-blade random, so the round trip runs on
        # the shipped GLSL pack and unpack.
        set(GRASS_CLUMP_GLSL ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/TerrainGrass/grass_clump.glsl)
        set(GRASS_BLADE_SHAPE_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassBladeShapeExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_BLADE_SHAPE_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_CLUMP_GLSL}
                                     -DOUT=${GRASS_BLADE_SHAPE_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_GRASS_BLADE_SHAPE_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_GRASS_BLADE_SHAPE_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_CLUMP_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the blade shape pack from grass_clump.glsl"
            VERBATIM)

        # And for the authored colour decode: the shared sRGB curve and the surface's read of the three
        # packed colours, so the decode is checked on the expressions that render.
        set(GRASS_SRGB_TRANSFER_GLSL
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes/srgb_transfer.glsl)
        set(GRASS_SRGB_TRANSFER_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassSrgbTransferExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_SRGB_TRANSFER_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_SRGB_TRANSFER_GLSL}
                                     -DOUT=${GRASS_SRGB_TRANSFER_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_SRGB_TRANSFER_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_SRGB_TRANSFER_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_SRGB_TRANSFER_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the sRGB transfer curve from srgb_transfer.glsl"
            VERBATIM)
        set(GRASS_SURFACE_GLSL
            ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/TerrainGrass/terrain_grass_surface.glsl)
        set(GRASS_COLOR_DECODE_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassColorDecodeExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_COLOR_DECODE_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_SURFACE_GLSL}
                                     -DOUT=${GRASS_COLOR_DECODE_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_GRASS_COLOR_DECODE_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_GRASS_COLOR_DECODE_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_SURFACE_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the grass colour decode from terrain_grass_surface.glsl"
            VERBATIM)

        # And for the light through a blade: the lighting model's far-face lobe and the surface's
        # translucency profile and transmitted albedo, so the energy rule is checked on the
        # expressions that render.
        set(GRASS_STANDARD_PBR_GLSL
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes/standard_pbr.glsl)
        set(GRASS_DIFFUSE_TRANSMISSION_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassDiffuseTransmissionExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_DIFFUSE_TRANSMISSION_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_STANDARD_PBR_GLSL}
                                     -DOUT=${GRASS_DIFFUSE_TRANSMISSION_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_DIFFUSE_TRANSMISSION_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_DIFFUSE_TRANSMISSION_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_STANDARD_PBR_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the diffuse transmission lobe from standard_pbr.glsl"
            VERBATIM)
        # And for the lighting model's dielectric reflectance, so the blade's specular weight is checked
        # through the F0 and the Fresnel terms: GE_FresnelF82 (the sun's lobe), GE_SchlickFresnel.
        set(GRASS_DIELECTRIC_FRESNEL_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassDielectricFresnelExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_DIELECTRIC_FRESNEL_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_STANDARD_PBR_GLSL}
                                     -DOUT=${GRASS_DIELECTRIC_FRESNEL_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_DIELECTRIC_FRESNEL_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_DIELECTRIC_FRESNEL_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_STANDARD_PBR_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the dielectric F0 and Fresnel from standard_pbr.glsl"
            VERBATIM)
        set(GRASS_TRANSLUCENCY_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassTranslucencyExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_TRANSLUCENCY_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_SURFACE_GLSL}
                                     -DOUT=${GRASS_TRANSLUCENCY_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_GRASS_TRANSLUCENCY_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_GRASS_TRANSLUCENCY_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_SURFACE_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the grass translucency from terrain_grass_surface.glsl"
            VERBATIM)

        # And for the blade's specular weight and the settle ranges it is read against.
        set(GRASS_BLADE_LOBE_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassBladeLobeExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_BLADE_LOBE_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_SURFACE_GLSL}
                                     -DOUT=${GRASS_BLADE_LOBE_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_GRASS_BLADE_LOBE_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_GRASS_BLADE_LOBE_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_SURFACE_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the blade specular weight from terrain_grass_surface.glsl"
            VERBATIM)

        add_executable(TerrainGrassStatsTests
            ${GRASS_DIFFUSE_TRANSMISSION_HEADER}
            ${GRASS_DIELECTRIC_FRESNEL_HEADER}
            ${GRASS_BLADE_LOBE_HEADER}
            ${GRASS_TRANSLUCENCY_HEADER}
            ${GRASS_SRGB_TRANSFER_HEADER}
            ${GRASS_COLOR_DECODE_HEADER}
            ${GRASS_BLADE_SHAPE_HEADER}
            ${GRASS_SPLAT_SOURCE_HEADER}
            ${GRASS_CANOPY_HEADER}
            ${TERRAIN_COLOR_MAP_HEADER}
            ${GRASS_BLADE_VERTEX_HEADER}
            ${GRASS_WIND_SEED_HEADER}
            Tests/TerrainGrassPlacementStatsTests.cpp
            Tests/GrassPlacementModelTests.cpp
            Tests/TerrainColorMapApplyTests.cpp
            Tests/GrassPlacementElisionTests.cpp
            Source/GrassPlacementElisionInputs.cpp
            Tests/GrassAtlasSplatResolveTests.cpp
            Tests/GrassDrawModeTests.cpp
            Tests/GrassWindSeedTests.cpp
            Tests/GrassBladeVertexTests.cpp
            Tests/GrassColorDecodeTests.cpp
            Tests/GrassBladeLightTests.cpp
            Source/TerrainGrassPlacementStats.cpp
        )
        set_target_properties(TerrainGrassStatsTests PROPERTIES FOLDER "TerrainGrass/Tests")
        target_link_libraries(TerrainGrassStatsTests PRIVATE
            GameEngineRendering
            FileSystemObjects
            AssetCore
            GTest::gtest
            GTest::gtest_main
        )
        # OBJECT->OBJECT object files are not spliced transitively on the VS
        # generator: splice Logger, Memory and Image (whose decode SkyRenderer calls)
        # in directly, as the CBT and Rendering suites do.
        target_sources(TerrainGrassStatsTests PRIVATE $<TARGET_OBJECTS:Logger> $<TARGET_OBJECTS:Memory>
            $<TARGET_OBJECTS:Image>)
        # GrassShaderSitePurity reads the shipped grass GLSL and fails on any seed path
        # that draws from the per-frame instance slot instead of the blade's site.
        target_compile_definitions(TerrainGrassStatsTests PRIVATE
            TERRAIN_GRASS_SHADER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Shaders/TerrainGrass"
            CBT_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT")
        target_include_directories(TerrainGrassStatsTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Include
            # GlslShim.h, the extracted-block header, and Terrain/TerrainTypes.h — the last so the
            # shader's mirror of kTerrainFlagAtlasBacked is asserted against the C++ constant
            # rather than against a literal.
            ${GRASS_GLSL_SHIM_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/generated
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Terrain/Include)
        ge_set_output_tests(TerrainGrassStatsTests)
        ge_stage_runtime_dependencies(TerrainGrassStatsTests)
        add_test(NAME TerrainGrassStatsTests COMMAND TerrainGrassStatsTests)
    endif()
endif()
