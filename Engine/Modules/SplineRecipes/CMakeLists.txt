cmake_minimum_required(VERSION 3.20)

project(SplineRecipes LANGUAGES CXX)

# What a spline recipe component generates (tiles, fence pieces, swept paths and
# roads, walls) and the terrain effects under a generated run: the controllers,
# the chunk commit, the rebuild gate, the sweep stations and the surface
# conform. Linked into Engine, so the editor, the Player and a game run one builder.
add_library(SplineRecipes OBJECT
    Source/FenceEmission.cpp
    Source/FenceMitreVariants.cpp
    Source/FencePieceReuse.cpp
    Source/FenceRecipeLayout.cpp
    Source/FenceSpanOverrides.cpp
    Source/PlacementGroundGap.cpp
    Source/RecipeBuildStates.cpp
    Source/RecipeControllers.cpp
    Source/RecipeValidation.cpp
    Source/SplineChunkCommit.cpp
    Source/SplineChunkSlots.cpp
    Source/SplineExtrudeController.cpp
    Source/SplineExtrudeSanitize.cpp
    Source/SplineFenceController.cpp
    Source/SplineFillRebuild.cpp
    Source/SplineGroundAuthority.cpp
    Source/SplineGroundCleanup.cpp
    Source/SplinePieceMaterial.cpp
    Source/SplinePlacementController.cpp
    Source/SplineRebuildGate.cpp
    Source/SplineRecipeSystem.cpp
    Source/SplineRecipesScriptApi.cpp
    Source/SplineSurfaceConform.cpp
    Source/SplineSweepStations.cpp
    Source/SplineWallBuild.cpp
    Source/SplineWallController.cpp
    Source/SplineWallSanitize.cpp
)

target_compile_features(SplineRecipes PUBLIC cxx_std_20)

target_include_directories(SplineRecipes
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        # Scripting/AbiMainThread.h: the script API's thread predicate.
        ${CMAKE_SOURCE_DIR}/Engine/Source
)

target_link_libraries(SplineRecipes
    PUBLIC
        Spline
        SplineGeometry
        SplineLayout
        SplineECS
        ECS
        ECSComponents
        Events
        GameEngineRendering
        AssetCore
        Types
        Mathematics
        Logger
)

# Engine's public headers (RenderServices.h, MeshGPURegistry.h, the scene
# raycast) and TerrainECS's (the modifier system, the height query) are read by
# the controllers. Headers only, no link: Engine is SHARED and links this module
# privately, so the symbols resolve inside Engine.dll (the CBTTerrainECS pattern).
target_include_directories(SplineRecipes PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:TerrainECS,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_definitions(SplineRecipes PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_COMPILE_DEFINITIONS>
)
set_target_properties(SplineRecipes PROPERTIES FOLDER "Engine/Modules")

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(SplineRecipesTests
        # The settle observation's surface term: a ConformMode=None recipe takes no
        # term, so terrain churn (tile streaming, modifier bakes) cannot starve its
        # rebuilds; conforming recipes pass the world digest through.
        Tests/SplineSurfaceObservationTests.cpp
        # The observe, settle and defer gate every spline recipe controller rebuilds
        # through, over the real class (the controllers need live RenderServices).
        Tests/SplineRebuildGateTests.cpp
        # Where the recipe system runs: exclusive, after the terrain bake and the spline caches.
        Tests/SplineRecipeScheduleTests.cpp
        # The build state of each recipe and the scene's settled summary, over a headless device.
        Tests/SplineRecipeBuildStateTests.cpp
        # What a fence conform ray may land on, end to end: the drape, the run fill
        # solved from the DRAPED length, and the per-station probe, over a running
        # engine's world with a terrain and a cube.
        Tests/FenceConformTargetTests.cpp
        # Same contract again for the generated-geometry recipe, whose every-field
        # equality guard is what keeps a mesh regenerating after a profile edit,
        # plus the NaN self-inequality guard shared with the fence. The sanitize TU
        # is a leaf over the component header only.
        Tests/SplineExtrudeRecipeTests.cpp
        # The swept wall's recipe contract: its exact size, every field in equality,
        # the sanitizer, and the corner a wall starts with, derived from its spline's
        # type when it is added and when it is reset.
        Tests/SplineWallRecipeTests.cpp
        # What a recipe's declared ground authority provisions, and the bar it is held
        # to: a provisioned run must bake the hand-authored pooled flatten to the BIT,
        # or there are two shapes of graded run to keep correct instead of one. Also
        # pins the steady-state no-write, without which the run re-bakes every frame.
        Tests/SplineGroundAuthorityTests.cpp
        # The extrude rebuild's slot reconciliation and chunk carving: chunk slots
        # are addressed by index so a mid-run empty chunk leaves a hole rather than
        # shifting its neighbour's mesh into its slot, and the carve must cover the
        # whole run — shared boundaries, no head hole, capped tail folded in. Leaf
        # TU (Types.h only).
        Tests/SplineChunkSlotTests.cpp
        # The chunk commit both generated-geometry controllers register their meshes
        # through: stable keys re-registered in place, holes, tail retirement and
        # the release of every key a build no longer reaches. Driven on a registry
        # with no device and a bare world.
        Tests/SplineChunkCommitTests.cpp
        # The fence's cut span variants from the layout's plan to the registry: the
        # unit SplineFenceController::Rebuild hands its plan to, on a registry with
        # no device — dedupe, stable keys, release, the byte budget, open meshes.
        Tests/FenceMitreVariantsTests.cpp
        # End-to-end coverage of the extrude's None-conform station path over the
        # RiverVignette water spline: arc sampling (Spline, via Engine.dll), chunk
        # carving and the strip sweep (SplineGeometry, via Engine.dll) together must
        # sweep every metre of the authored curve.
        Tests/SplineExtrudeCoverageTests.cpp
        # The water fill through its rebuild entry point, against a real terrain: the
        # ROI lands on the terrain's own sample lattice, and EndTaperMetres is
        # IGNORED in FitToBanks (asserted as byte identity, with the sea-level floor
        # as the positive control that the comparison can fail at all).
        Tests/SplineFillRebuildTests.cpp
        # The refusal warnings themselves: a count and a magnitude locate nothing, so
        # each line has to carry the world position of the sample it is worst at.
        Tests/WaterFillDiagnosticsReportTests.cpp
        # Where the region STOPS, against the IslandVignette terrain rebuilt from its
        # own scene: no wet corner may stand above the elevation at which its region
        # would escape, and the region's edge over falling ground is that rim rather
        # than the MaxHalfWidth corridor.
        Tests/WaterFillTerminationTests.cpp
        # The composed-ground query and the signed ground-to-underside gap, against a
        # flat synthetic base so a modifier is the only thing that can move a reading.
        # Pieces are rotated, scaled and mirrored — a level piece is the one pose where
        # a box around a piece and the piece itself agree — and both residencies run.
        Tests/PlacementGroundGapTests.cpp
        # Byte-identity oracle for the Channel sweep over the RiverVignette run. The region
        # fill takes over FitToBanks only; this pins Channel's vertices, indices and
        # bounds to a digest captured before the fill landed, tapered and untapered.
        Tests/SplineChannelParityTests.cpp
        # The same byte-identity bar for every path and road the generic sweep builds
        # (Bevel, Crown), through the controller's own station stream: open and
        # closed, Linear and smooth, level and sloped ground, a transformed placer.
        Tests/SplineSweepParityTests.cpp
        # The swept wall's corners, closed loops, base, steps and texture coordinates,
        # held at the numbers the swept-wall design names, through the wall
        # controller's own pipeline on the same station stream.
        Tests/SplineSweepWallTests.cpp
        # Piece material binding shared by both spline controllers: which material a
        # spawned piece ends up with. The bind half touches no RenderServices, so the
        # override contract is pinned here as a leaf TU over ModelRenderSetup.
        Tests/SplinePieceMaterialTests.cpp
        # The fence controller's emission list: stations, spans, then the crest row
        # in layout order, each role under its own mesh signature base, and a
        # registered crest at its wall's length scale. Leaf TU over plain pool
        # entries; the controller needs a live RenderServices to resolve them.
        Tests/FenceEmissionTests.cpp
        # A fence's piece entities survive a rebuild by what they are, not where
        # they fall in the list, so a picked span keeps its identity.
        Tests/FencePieceReuseTests.cpp
        # What a fence layout reads from its recipe, the way the controller reads it.
        Tests/FenceRecipeLayoutTests.cpp
        # Writing a fence's span overrides: gate, pinned piece, clear, full table.
        Tests/FenceSpanOverridesTests.cpp
        # Fixtures here initialize the engine; this shuts it down before the process exits.
        ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp
    )
    # TestTempDir.h, shared across the test trees.
    target_include_directories(SplineRecipesTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests)
    target_link_libraries(SplineRecipesTests PRIVATE Engine SharedHeadlessDeviceTestSupport GTest::gtest GTest::gtest_main)
    set_target_properties(SplineRecipesTests PROPERTIES FOLDER "Engine/Tests")
    ge_set_output_tests(SplineRecipesTests)
    ge_stage_runtime_dependencies(SplineRecipesTests)
    add_test(NAME SplineRecipesTests COMMAND $<TARGET_FILE:SplineRecipesTests>)

    # Opt-in: the water fill's investigation instruments, which run against scenes in other content
    # repositories (GE_WATER_LIFT_REPRO_SCENE, GE_SCENE_WATER_GATE) and skip without them. Not
    # registered with CTest and run by no gate: build WaterFillInvestigationTests and run it with the
    # scene set.
    add_executable(WaterFillInvestigationTests
        # The lifted river over an authored island terrain: does the lift enclose dry ground?
        Tests/WaterFillLiftReproTests.cpp
        # The fill applied to an authored scene file: load, provision the terrain data the schema
        # does not carry, bake the modifier stack, and run the fill over every FitToBanks recipe.
        Tests/SceneWaterFillGateTests.cpp
        ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownEnvironment.cpp
    )
    target_link_libraries(WaterFillInvestigationTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    set_target_properties(WaterFillInvestigationTests PROPERTIES FOLDER "Engine/Tests")
    ge_set_output_tests(WaterFillInvestigationTests)
    ge_stage_runtime_dependencies(WaterFillInvestigationTests)
endif()
