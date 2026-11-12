// The Inspector notice on a planet with edits on a GPU without 64-bit shader integers: that GPU
// reads no planet height page layer, so it draws the planet without its edits (#3150).

#include <gtest/gtest.h>

#include "InspectorLayoutFixture.h"
#include "Mathematics/Vector3.h"
#include "TerrainECS/Planet/PlanetPageRequester.h"
#include "TerrainECS/Planet/PlanetSculptDab.h"
#include "TerrainECS/TerrainService.h"
#include "Terrain/Planet/PlanetEditsNotice.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using GameEngine::TerrainECS::TerrainService;

TEST(PlanetEditsNotice, OnlyAPlanetWithEditsTheGpuCannotDrawShowsTheNotice)
{
    if (TerrainService::IsInitialized())
        TerrainService::Shutdown();
    TerrainService::Initialize();
    TerrainService& service = TerrainService::Get();
    constexpr uint32 kPlanet = 7;

    EXPECT_EQ(Editor::DescribeUndrawnPlanetEdits(service, kPlanet, true), "") << "a planet without edits";

    const uint32 faceSamples = TerrainECS::PlanetFaceSamples(500.0, 1.0f);
    service.ApplyPlanetSculptDab(kPlanet, faceSamples,
                                 TerrainECS::PlanetSculptDab(Mathematics::Vector3(0.0f, 1.0f, 0.0f), 0.05f, 2.0f));
    EXPECT_EQ(Editor::DescribeUndrawnPlanetEdits(service, kPlanet, true),
              "This GPU cannot show planet edits. The planet draws without its sculpt and modifiers. Your edits "
              "are kept and saved. Open the project on a GPU that supports 64-bit shader integers to see them.");
    EXPECT_EQ(Editor::DescribeUndrawnPlanetEdits(service, kPlanet, false), "") << "the wide arm draws the edits";

    TerrainService::Shutdown();
}

TEST(PlanetEditsNotice, OnlyASurfaceRequestMadeCoarserToFitTheSlotsShowsTheReducedDetailNotice)
{
    EXPECT_EQ(Editor::DescribeReducedPlanetSurface(TerrainECS::PageSlotFit{0u, 3000u}, 4096u), "")
        << "a request that fits";
    EXPECT_EQ(Editor::DescribeReducedPlanetSurface(TerrainECS::PageSlotFit{1u, 5210u}, 4096u),
              "Less ground detail in this view: it needs 5,210 surface pages and this platform holds 4,096.");
}

// The fit notice follows the fit each frame: 3,228 pages, then 558, then hidden when the request fits. A frame
// with the same text does not set the notice again.
TEST(PlanetEditsNotice, TheReducedDetailNoticeFollowsTheFitEachFrame)
{
    auto parent = std::make_unique<UIElement>();
    TerrainECS::PageSlotFit fit{2u, 3228u};
    const uint32 slots = 512u;
    std::vector<std::function<void()>> frameRefreshCallbacks;
    EditorUI::InspectorNotice* notice = Editor::AddReducedPlanetSurfaceNotice(
        parent.get(), [&fit, slots]() { return Editor::DescribeReducedPlanetSurface(fit, slots); },
        &frameRefreshCallbacks);
    ASSERT_NE(notice, nullptr);
    auto* label = dynamic_cast<Label*>(InspectorLayoutTesting::FindByClass(notice, "inspector-notice-text"));
    ASSERT_NE(label, nullptr);
    const auto runFrame = [&frameRefreshCallbacks]()
    {
        for (const std::function<void()>& callback : frameRefreshCallbacks)
            callback();
    };

    EXPECT_FALSE(notice->HasClass("hidden")) << "a request made coarser";
    EXPECT_EQ(label->GetText(),
              "Less ground detail in this view: it needs 3,228 surface pages and this platform holds 512.");

    fit = TerrainECS::PageSlotFit{1u, 558u};
    runFrame();
    EXPECT_EQ(label->GetText(),
              "Less ground detail in this view: it needs 558 surface pages and this platform holds 512.");

    label->SetText("set by the test");
    runFrame();
    EXPECT_EQ(label->GetText(), "set by the test") << "a frame with the same fit sets the notice again";

    fit = TerrainECS::PageSlotFit{0u, 0u};
    runFrame();
    EXPECT_TRUE(notice->HasClass("hidden")) << "the request fits again";
}
