#pragma once

#include "Types/Types.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{
class UIElement;
namespace EditorUI
{
class InspectorNotice;
} // namespace EditorUI
namespace TerrainECS
{
class TerrainService;
struct PageSlotFit;
} // namespace TerrainECS
} // namespace GameEngine

namespace GameEngine::Editor
{

// The text that tells the author that a device without 64-bit shader integers (`narrowHeap`) draws
// planet `planet` (its terrain entity id) without its sculpt and modifier edits. Empty when
// `narrowHeap` is false or the planet has no edits.
std::string DescribeUndrawnPlanetEdits(const TerrainECS::TerrainService& service, uint32 planet, bool narrowHeap);

// Shows DescribeUndrawnPlanetEdits for the running device as an information notice under `parent`
// when the text is not empty.
void AddPlanetEditsNotice(UIElement* parent, uint32 planet);

// Shows TerrainService::PlanetSculptRefusal for planet `planet`, whose height pages have `faceSamples`
// level-0 samples a side at a level-0 texel of `level0Texel` meters, as a warning notice under `parent`
// when the text is not empty.
void AddPlanetSculptRefusalNotice(UIElement* parent, uint32 planet, uint32 faceSamples, float32 level0Texel);

// The text that tells the author that a planet's ground shows less detail because the view needs more
// surface pages than the field's `slots` hold (`fit` from TerrainSurfacePages::PlanetFit). Empty while
// the request fits.
std::string DescribeReducedPlanetSurface(const TerrainECS::PageSlotFit& fit, uint32 slots);

// An information notice under `parent` with the text that `describe` returns, hidden while the text is empty.
// A callback in `frameRefreshCallbacks` calls `describe` each frame and sets the notice again only when the
// text changes.
EditorUI::InspectorNotice* AddReducedPlanetSurfaceNotice(UIElement* parent, std::function<std::string()> describe,
                                                         std::vector<std::function<void()>>* frameRefreshCallbacks);

// AddReducedPlanetSurfaceNotice with DescribeReducedPlanetSurface for planet `planet`'s last surface request
// and the field's slots.
void AddPlanetSurfaceFitNotice(UIElement* parent, uint32 planet,
                               std::vector<std::function<void()>>* frameRefreshCallbacks);

} // namespace GameEngine::Editor
