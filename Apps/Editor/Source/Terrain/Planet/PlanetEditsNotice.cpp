#include "Terrain/Planet/PlanetEditsNotice.h"

#include "CBTTerrainECS/CBTRenderFeature.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "TerrainECS/Planet/PlanetPageRequester.h"
#include "TerrainECS/TerrainPages.h"
#include "TerrainECS/TerrainService.h"
#include "Types/StringUtils.h"
#include "UI/Controls/InspectorNotice.h"

#include <format>

namespace GameEngine::Editor
{

std::string DescribeUndrawnPlanetEdits(const TerrainECS::TerrainService& service, uint32 planet, bool narrowHeap)
{
    if (!narrowHeap)
        return {};
    const TerrainECS::TerrainService::PlanetPageOverlay* overlay = service.FindPlanetPageOverlay(planet);
    if (!overlay || (!overlay->Layer && !overlay->Modifiers))
        return {};
    return "This GPU cannot show planet edits. The planet draws without its sculpt and modifiers. Your edits are "
           "kept and saved. Open the project on a GPU that supports 64-bit shader integers to see them.";
}

void AddPlanetEditsNotice(UIElement* parent, uint32 planet)
{
    const TerrainECS::TerrainService* service = TerrainECS::TerrainService::TryGet();
    const Engine::Renderer::RenderServices* renderServices = EngineCore::GetInstance().GetRenderServices();
    const CBTTerrainECS::CBTRenderFeature* feature =
        renderServices ? renderServices->GetFeature<CBTTerrainECS::CBTRenderFeature>() : nullptr;
    if (!parent || !service || !feature || !feature->IsInitialized())
        return;
    const std::string text = DescribeUndrawnPlanetEdits(*service, planet, feature->UsesNarrowHeap());
    if (!text.empty())
        InspectorUI::AddNotice(parent, text, EditorUI::InspectorNotice::Kind::Information);
}

void AddPlanetSculptRefusalNotice(UIElement* parent, uint32 planet, uint32 faceSamples, float32 level0Texel)
{
    const TerrainECS::TerrainService* service = TerrainECS::TerrainService::TryGet();
    if (!parent || !service)
        return;
    const std::string text = service->PlanetSculptRefusal(planet, faceSamples, level0Texel);
    if (!text.empty())
        InspectorUI::AddNotice(parent, text, EditorUI::InspectorNotice::Kind::Warning);
}

std::string DescribeReducedPlanetSurface(const TerrainECS::PageSlotFit& fit, uint32 slots)
{
    if (fit.Passes == 0)
        return {};
    return std::format("Less ground detail in this view: it needs {} surface pages and this platform holds {}.",
                       FormatGroupedInteger(fit.FullDetailPages), FormatGroupedInteger(slots));
}

namespace
{

// Sets `notice` to `text`, hidden while `text` is empty, when `text` differs from `shown`, the text it shows.
void RefreshReducedPlanetSurfaceNotice(EditorUI::InspectorNotice& notice, const std::string& text, std::string& shown)
{
    if (text == shown)
        return;
    shown = text;
    notice.SetText(text);
    if (text.empty())
        notice.AddClass("hidden");
    else
        notice.RemoveClass("hidden");
}

// DescribeReducedPlanetSurface for planet `planet`'s last surface request; empty without a page field.
std::string DescribePlanetSurfaceFit(uint32 planet)
{
    const TerrainECS::TerrainService* service = TerrainECS::TerrainService::TryGet();
    const TerrainECS::TerrainPages* pages = service ? service->FindTerrainPages() : nullptr;
    if (!pages)
        return {};
    return DescribeReducedPlanetSurface(pages->Surface().PlanetFit(planet), pages->Surface().Slots());
}

} // namespace

EditorUI::InspectorNotice* AddReducedPlanetSurfaceNotice(UIElement* parent, std::function<std::string()> describe,
                                                         std::vector<std::function<void()>>* frameRefreshCallbacks)
{
    if (!parent || !describe)
        return nullptr;
    EditorUI::InspectorNotice* notice =
        InspectorUI::AddNotice(parent, std::string{}, EditorUI::InspectorNotice::Kind::Information);
    if (!notice)
        return nullptr;
    notice->SetId("planet-surface-fit-notice");
    notice->AddClass("hidden");
    std::string shown;
    RefreshReducedPlanetSurfaceNotice(*notice, describe(), shown);
    if (frameRefreshCallbacks)
        frameRefreshCallbacks->push_back([notice, describe = std::move(describe), shown]() mutable
                                         { RefreshReducedPlanetSurfaceNotice(*notice, describe(), shown); });
    return notice;
}

void AddPlanetSurfaceFitNotice(UIElement* parent, uint32 planet,
                               std::vector<std::function<void()>>* frameRefreshCallbacks)
{
    const TerrainECS::TerrainService* service = TerrainECS::TerrainService::TryGet();
    if (!service || !service->FindTerrainPages())
        return;
    AddReducedPlanetSurfaceNotice(parent, [planet]() { return DescribePlanetSurfaceFit(planet); },
                                  frameRefreshCallbacks);
}

} // namespace GameEngine::Editor
