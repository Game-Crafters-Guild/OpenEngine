// The planet sculpt stroke on the undo stack: a stroke through TerrainService's planet layer capture
// (the sequence TerrainBrushTool's Begin/EndSphereStroke runs) is one PlanetSculptStrokeCommand; undo
// and redo put back the layer's composed pages byte for byte and advance the version the height
// pages compare, with the changed face rects.

#include <gtest/gtest.h>

#include "Terrain/Planet/PlanetSculptStrokeCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "TerrainECS/Planet/PlanetPageRequester.h"
#include "TerrainECS/Planet/PlanetSculptDab.h"
#include "TerrainECS/TerrainService.h"

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace
{
using namespace GameEngine;
using GameEngine::Editor::PlanetSculptStrokeCommand;
using GameEngine::Editor::UndoRedoService;
using GameEngine::PageStreaming::HeightPageLayerPage;
using GameEngine::PageStreaming::PageAddress;
using GameEngine::TerrainECS::PlanetSculptDab;
using GameEngine::TerrainECS::TerrainService;

constexpr uint32 kPlanet = 7;
constexpr float64 kPlanetRadius = 500.0;
constexpr float32 kTexel = 1.0f;
constexpr float32 kSmallBrush = 0.05f; // 25 m at 500 m: a level-0 write
constexpr float32 kLargeBrush = 0.6f;  // 300 m: a write at a coarser level

using ComposedPages = std::vector<std::pair<PageAddress, std::vector<float32>>>;

// Every page the planet's layer adds to, composed on zeros.
ComposedPages Compose(const TerrainService& svc, uint32 faceSamples)
{
    ComposedPages out;
    const TerrainService::PlanetPageOverlay* overlay = svc.FindPlanetPageOverlay(kPlanet);
    if (!overlay || !overlay->Layer)
        return out;
    const std::vector<PageStreaming::PageStoreLevel> levels =
        PageStreaming::BuildPageStoreLevels(faceSamples, faceSamples);
    std::vector<float32> samples(PageStreaming::kPageSampleCount);
    for (uint8 face = 0; face < PageStreaming::kPageStoreMaxFaces; ++face)
        for (uint32 level = 0; level < levels.size(); ++level)
            for (uint32 z = 0; z < levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < levels[level].PagesX; ++x)
                {
                    const PageAddress address{face, static_cast<uint8>(level), x, z};
                    std::fill(samples.begin(), samples.end(), 0.0f);
                    if (overlay->Layer->ApplyToPage(address, samples))
                        out.emplace_back(address, samples);
                }
    return out;
}

class PlanetSculptStrokeCommandTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
        m_FaceSamples = TerrainECS::PlanetFaceSamples(kPlanetRadius, kTexel);
        ASSERT_GT(m_FaceSamples, 0u);
    }
    void TearDown() override
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }

    // One stroke of dabs centered on `centers`, committed to `undo` as one already-applied command.
    // Returns whether an entry was pushed.
    bool Stroke(UndoRedoService& undo, std::initializer_list<Mathematics::Vector3> centers, float32 angularRadius,
                float32 amplitude)
    {
        TerrainService& svc = TerrainService::Get();
        svc.BeginPlanetSculptStroke(kPlanet, m_FaceSamples);
        for (const Mathematics::Vector3& center : centers)
            svc.ApplyPlanetSculptDab(kPlanet, m_FaceSamples, PlanetSculptDab(center, angularRadius, amplitude));
        std::vector<HeightPageLayerPage> before = svc.TakePlanetSculptStroke(kPlanet);
        if (before.empty())
            return false;
        std::vector<HeightPageLayerPage> after = svc.CopyPlanetSculptPages(kPlanet, before);
        undo.CommitAlreadyApplied(
            std::make_unique<PlanetSculptStrokeCommand>(&svc, kPlanet, std::move(before), std::move(after)));
        return true;
    }

    uint32 m_FaceSamples = 0;
};

} // namespace

// A stroke of several dabs is one undo entry; undo and redo restore the composed pages byte for byte,
// for a level-0 stroke and for a stroke that writes a coarser level.
TEST_F(PlanetSculptStrokeCommandTests, StrokeUndoRedoRestoresTheComposedPagesByteForByte)
{
    TerrainService& svc = TerrainService::Get();
    UndoRedoService undo;
    ASSERT_TRUE(Stroke(undo, {{0.0f, 1.0f, 0.0f}}, kSmallBrush, 2.0f));
    const ComposedPages pre = Compose(svc, m_FaceSamples);
    ASSERT_FALSE(pre.empty());

    for (const float32 radius : {kSmallBrush, kLargeBrush})
    {
        const ComposedPages before = Compose(svc, m_FaceSamples);
        const std::size_t entries = undo.GetUndoCount();
        ASSERT_TRUE(Stroke(undo, {{0.0f, 1.0f, 0.0f}, {0.05f, 1.0f, 0.0f}, {0.1f, 1.0f, 0.05f}}, radius, -1.5f));
        EXPECT_EQ(undo.GetUndoCount(), entries + 1u) << "one stroke of three dabs is one undo entry";
        EXPECT_STREQ(undo.PeekUndoName(), "Planet Sculpt Stroke");
        const ComposedPages after = Compose(svc, m_FaceSamples);
        ASSERT_NE(after, before) << "the stroke changes the layer";

        undo.Undo();
        EXPECT_EQ(Compose(svc, m_FaceSamples), before) << "undo is not byte exact at radius " << radius;
        undo.Redo();
        EXPECT_EQ(Compose(svc, m_FaceSamples), after) << "redo is not byte exact at radius " << radius;
        undo.Undo();
    }
    EXPECT_EQ(Compose(svc, m_FaceSamples), pre);
}

// Undo and redo each give the layer a new version and name the faces they change, so the height
// pages that read the stroke are rewritten.
TEST_F(PlanetSculptStrokeCommandTests, UndoAndRedoAdvanceTheVersionWithTheChangedRects)
{
    TerrainService& svc = TerrainService::Get();
    UndoRedoService undo;
    ASSERT_TRUE(Stroke(undo, {{0.0f, 1.0f, 0.0f}}, kSmallBrush, 3.0f));
    const TerrainService::PlanetPageOverlay* overlay = svc.FindPlanetPageOverlay(kPlanet);
    ASSERT_NE(overlay, nullptr);

    for (const bool redo : {false, true})
    {
        const uint64 version = overlay->Version;
        if (redo)
            undo.Redo();
        else
            undo.Undo();
        EXPECT_GT(overlay->Version, version);
        EXPECT_TRUE(std::ranges::any_of(overlay->Changed, [](const auto& rect) { return rect.has_value(); }))
            << (redo ? "redo" : "undo") << " names no changed rect";
    }
}

// The capture holds only the pages the stroke changed, and on a new layer each pre-image says "no page".
TEST_F(PlanetSculptStrokeCommandTests, TheCaptureHoldsOnlyTheChangedPages)
{
    TerrainService& svc = TerrainService::Get();
    svc.BeginPlanetSculptStroke(kPlanet, m_FaceSamples);
    svc.ApplyPlanetSculptDab(kPlanet, m_FaceSamples, PlanetSculptDab({0.0f, 1.0f, 0.0f}, kSmallBrush, 2.0f));
    const std::vector<HeightPageLayerPage> before = svc.TakePlanetSculptStroke(kPlanet);
    ASSERT_FALSE(before.empty());
    EXPECT_LE(before.size(), Compose(svc, m_FaceSamples).size()) << "the capture holds a page the stroke did not change";
    for (const HeightPageLayerPage& page : before)
        EXPECT_TRUE(page.Deltas.empty()) << "a new layer held no page before the stroke";
}

TEST_F(PlanetSculptStrokeCommandTests, AStrokeWithNoStrengthRecordsNothing)
{
    UndoRedoService undo;
    EXPECT_FALSE(Stroke(undo, {{0.0f, 1.0f, 0.0f}}, kSmallBrush, 0.0f));
    EXPECT_EQ(undo.GetUndoCount(), 0u);
}
