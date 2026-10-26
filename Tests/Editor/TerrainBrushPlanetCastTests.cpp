#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// ---- The sculpt brush keeps its own planet cast ----
//
// The brush hands a cast's normal to ApplySphereDab as the surface DIRECTION to sculpt, so on
// a planet it needs a cast that answers only about the planet. Scene::RaycastTerrain is the
// combined scene-level cast: it prefers a planar terrain's hit (whose normal is the planar
// convention, world +Y — the north pole as a sculpt direction), it is gated on the CBT render
// feature's own domain state, and it is capped by the caller's pick reach. Routing the brush
// through it moved sculpt dabs. Unifying the two is a change that has to prove equivalence
// first.
//
// Both of the tool's cast entry points must honour that: RaycastSurface (the pointer path's
// camera ray) and ProbeSurfaceAt (the automation path's probe ray). Each dispatches on
// IsSphericalPlanet, so the lock is positional — the planet branch has to come first and cast
// through RaycastPlanet, leaving the combined cast reachable only once the planet case is
// ruled out.
//
// TerrainBrushTool.cpp is in no test target (its dependency cone pulls in the undo service,
// change notifications and zone authoring) and Scene::RaycastActivePlanet reaches
// RenderServices through the Engine singleton, so this contract has no reachable behavioural
// seam. It is locked at the source level, the way PanelDefaultTabIconTests locks the panel
// tab-icon defaults.
TEST(TerrainPickingRay, SphereBrushCursorCastsThroughTheBrushsOwnPlanetRaycast)
{
    const std::filesystem::path brushSource = std::filesystem::path(GE_EDITOR_SOURCE_DIR) /
                                              "Source" / "SceneView" / "TerrainBrushTool.cpp";
    std::ifstream in(brushSource, std::ios::binary);
    ASSERT_TRUE(in.good()) << "cannot read " << brushSource.string();
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();

    // Each entry point's body, delimited by the next function definition after it.
    struct CastEntryPoint
    {
        const char* Signature;
        const char* NextSignature;
    };
    const CastEntryPoint entryPoints[] = {
        {"bool TerrainBrushTool::RaycastSurface", "\nbool TerrainBrushTool::ProbeSurfaceAt"},
        {"bool TerrainBrushTool::ProbeSurfaceAt", "\nbool TerrainBrushTool::ApplyStrokePhase"},
    };

    for (const CastEntryPoint& entry : entryPoints)
    {
        const std::size_t bodyBegin = source.find(entry.Signature);
        ASSERT_NE(bodyBegin, std::string::npos)
            << entry.Signature << " is gone; re-point this lock at whatever replaced it";
        const std::size_t bodyEnd = source.find(entry.NextSignature, bodyBegin);
        ASSERT_NE(bodyEnd, std::string::npos)
            << entry.NextSignature << " is gone; re-point this lock";
        const std::string body = source.substr(bodyBegin, bodyEnd - bodyBegin);

        const std::size_t domainGate = body.find("IsSphericalPlanet(");
        const std::size_t planetCast = body.find("RaycastPlanet(");
        const std::size_t terrainCast = body.find("RaycastTerrain(");

        EXPECT_NE(domainGate, std::string::npos)
            << entry.Signature << " must dispatch on the active terrain's domain";
        EXPECT_NE(planetCast, std::string::npos)
            << entry.Signature << " must cast through TerrainBrushTool::RaycastPlanet on a planet";
        EXPECT_LT(domainGate, planetCast)
            << entry.Signature << " must rule the domain BEFORE casting";
        EXPECT_LT(planetCast, terrainCast)
            << entry.Signature << " reaches the combined scene terrain cast before the planet "
               "cast: that cast returns a planar terrain's up-normal as the sculpt direction, is "
               "gated on the CBT feature's domain state, and is capped by the pick reach";
    }
}
