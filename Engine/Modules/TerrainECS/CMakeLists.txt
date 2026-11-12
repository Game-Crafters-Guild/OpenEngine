cmake_minimum_required(VERSION 3.20)

project(TerrainECS LANGUAGES CXX)

add_library(TerrainECS OBJECT
    Source/TerrainService.cpp
    Source/TerrainHeightFunction/RevocableTerrainHeightFunction.cpp
    Source/TerrainHeightFunction/TerrainHeightFunctionInstances.cpp
    Source/TerrainHeightFunction/TerrainHeightFunctionRegistry.cpp
    Source/CoarseTileStandIn.cpp
    Source/TerrainBakeKeyBuilder.cpp
    Source/BakeWarningLatch.cpp
    Source/PlanarHeightfieldEditCursor.cpp
    Source/TerrainHandleHygiene.cpp
    Source/TerrainGpuBake.cpp
    Source/TerrainEntityProvisioning.cpp
    Source/TerrainRenderFeature.cpp
    Source/GrassWindVolumeRing.cpp
    Source/GrassWindVolumeExtractor.cpp
    Source/TerrainMaterialAuthoring.cpp
    Source/TerrainMaterialLibraryCache.cpp
    Source/TerrainUploadNode.cpp
    Source/Planet/PlanetChart.cpp
    Source/Planet/PlanetLayerCap.cpp
    Source/Planet/PlanetPageBodyFill.cpp
    Source/TerrainAtlas.cpp
    Source/AtlasTileNormal.cpp
    Source/TerrainSizingPlan.cpp
    Source/RawHeightmap.cpp
    Source/AtlasResidencyController.cpp
    Source/PageLevelRule.cpp
    Source/Planet/PlanetPageRequester.cpp
    Source/Planet/PlanetSculptDab.cpp
    Source/Planet/PlanetSurfacePageProvider.cpp
    Source/PageTable.cpp
    Source/PackedPageTable.cpp
    Source/PagedHeightSampler.cpp
    Source/SurfaceHeightQuery.cpp
    Source/TerrainSkyline.cpp
    Source/TerrainHeightSource.cpp
    Source/TerrainPages.cpp
    Source/TerrainPageFeature.cpp
    Source/TerrainPageTableRing.cpp
    Source/TerrainHeightPageDriver.cpp
    Source/TerrainRenderedViews.cpp
    Source/HeightPageStoreLoader.cpp
    Source/Systems/TerrainExtractionSystem.cpp
    Source/Systems/TerrainPhysicsSystem.cpp
    Source/Systems/RegisterTerrainSystems.cpp
    Source/Systems/TerrainWorldHooks.cpp
    Source/Systems/TerrainModifierSystem.cpp
    Source/Systems/TerrainModifierPageOverlay.cpp
    Source/Systems/TerrainModifierSurfacePages.cpp
    Source/TerrainSurface/TerrainSurfaceBakeQueue.cpp
    Source/TerrainSurface/TerrainSurfacePageBaker.cpp
    Source/TerrainSurface/TerrainSurfacePageDriver.cpp
    Source/TerrainSurface/TerrainSurfacePages.cpp
    Source/TerrainSurface/TerrainSurfaceRulePresets.cpp
    Source/TerrainSurface/TerrainSurfaceRuleRow.cpp
    Source/TerrainSurface/TerrainSurfaceStore.cpp
    Source/TileStreamingManager.cpp
    Source/TerrainModifierComponents.cpp
    Source/TerrainColorMap/TerrainColorMap.cpp
    Source/TerrainColorMap/TerrainColorMapBaker.cpp
    Source/TerrainGrass/TerrainGrassField.cpp
    Source/TerrainGrass/TerrainGrassCompose.cpp
    Source/TerrainFieldUploader.cpp
    Source/TerrainRowBands.cpp
    Source/TerrainRoutePolyline.cpp
    Source/AreaFlatten/AreaReferenceSurface.cpp
    Source/AreaFlatten/AreaReferenceSurfaceCache.cpp
    Source/TerrainZonePayload.cpp
    Source/TerrainZoneAuthoring.cpp
    Source/Scene/TerrainSceneSchemas.cpp
    # MPL-2.0, isolated: the description-derived erosion technique lives here
    # and nowhere else. See ThirdParty/RunevisionErosionFilter/UPSTREAM.md.
    Source/Erosion/ErosionFilter.cpp
)

target_compile_features(TerrainECS PUBLIC cxx_std_20)

target_include_directories(TerrainECS
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(TerrainECS
    PUBLIC
        Terrain
        ECS
        ECSComponents
        Types
        Mathematics
        Noise
        Logger
        GameEngineRendering
        # TerrainService.h exposes the asset-reload lane + decoded heightmap
        # cache (AssetReloadInvalidator / GUID members) in its public surface.
        AssetCore
        # The height page store, its cooker and loader (the paged height source); SurfaceHeightQuery.h
        # and TerrainService.h expose its page types.
        PageStreaming
    PRIVATE
        # Engine intentionally NOT listed: Engine is now SHARED (Phase 1) and
        # PRIVATE-links TerrainECS itself. A direct TerrainECS -> Engine link
        # would create a SHARED↔STATIC cycle. Engine's symbols (RenderServices,
        # Material, ShadowMapRenderFeature, etc.) get resolved when
        # Engine.dll links TerrainECS.lib. Headers come in via the generator
        # expression below.
        JobSystem
        FileSystem
        Physics
        PhysicsECS
        Spline
        SplineECS
)

# Engine's public headers (Engine/Include/Engine/Rendering/Material.h etc.)
# are needed by TerrainECS's implementation. We can't link Engine (cycle), so
# reach into its INTERFACE include directories directly — gets the headers
# without adding a link dependency.
target_include_directories(TerrainECS PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_INCLUDE_DIRECTORIES>
)

# Same reason we can't link Engine: pull its INTERFACE compile definitions
# (notably GE_ENABLE_CPU_PROFILING) so GE_CPU_PROFILE_SCOPE in the terrain
# systems actually records, and ScopedCpuProfile has one consistent definition
# across Engine and TerrainECS (no ODR mismatch).
target_compile_definitions(TerrainECS PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_COMPILE_DEFINITIONS>
)

# The planet reads (CBTPlanetShading.h, CBTSphereFaceMap.h, CBTLayout.h) are
# CBTTerrain's device-free, header-only CPU mirror of the GPU's sphere math. Pull
# CBTTerrain's public headers without a link dependency (no cycle: CBTTerrain does
# not depend on TerrainECS).
target_include_directories(TerrainECS PRIVATE
    $<TARGET_PROPERTY:CBTTerrain,INTERFACE_INCLUDE_DIRECTORIES>
)
