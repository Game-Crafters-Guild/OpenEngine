#include "DebugServer/Terrain/TerrainDebugHandlers.h"

#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Terrain/Terrain.h"
#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "ECS/ECSTemplates.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/VectorOps.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "TerrainECS/PlanetPageRequester.h"
#include "TerrainECS/TerrainService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

constexpr uint32 kMaxBumpRadiusSamples = 1024u;

// A raised cosine of `height` meters at level-0 sample (x, z), zero at `radius` samples and past it.
std::vector<float32> RaisedCosine(const PageStreaming::PageSampleRect& rect, uint32 x, uint32 z, uint32 radius,
                                  float32 height)
{
    std::vector<float32> delta(static_cast<std::size_t>(rect.Width()) * rect.Height(), 0.0f);
    for (uint32 sz = rect.MinZ; sz <= rect.MaxZ; ++sz)
        for (uint32 sx = rect.MinX; sx <= rect.MaxX; ++sx)
        {
            const float32 dx = static_cast<float32>(sx) - static_cast<float32>(x);
            const float32 dz = static_cast<float32>(sz) - static_cast<float32>(z);
            const float32 t = std::sqrt(dx * dx + dz * dz) / static_cast<float32>(radius);
            if (t < 1.0f)
                delta[static_cast<std::size_t>(sz - rect.MinZ) * rect.Width() + (sx - rect.MinX)] =
                    height * 0.5f * (1.0f + std::cos(Mathematics::Pi * t));
        }
    return delta;
}

json PlacePlanetPageOverlay(const EditorDebugServer::RequestContext& ctx)
{
    auto* service = TerrainECS::TerrainService::TryGet();
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!service || !world)
        return Editor::RefuseRequest("No terrain service or world");
    const ECS::EntityHandle entity = CBTTerrainECS::FindActiveTerrainEntity(*world);
    const auto* terrain = entity.IsValid() ? world->GetComponent<Components::Terrain>(entity) : nullptr;
    if (!terrain || terrain->Domain != Components::TerrainDomain::Spherical)
        return Editor::RefuseRequest("No active planet: open a scene with a spherical terrain first");
    if (ctx.params.value("clear", false))
    {
        service->ClearPlanetPageLayer(entity.id);
        return json{{"ok", true}, {"cleared", true}};
    }

    const float32 texel = terrain->SamplesPerMeter > 0.0f ? 1.0f / terrain->SamplesPerMeter : 0.0f;
    const uint32 samples = TerrainECS::PlanetFaceSamples(terrain->PlanetRadius, texel);
    if (samples == 0)
        return Editor::RefuseRequest("The planet does not stream height pages (see get_terrain_stats)");
    const uint32 face = ctx.params.value("face", 0u);
    const uint32 x = ctx.params.value("x", samples / 2u);
    const uint32 z = ctx.params.value("z", samples / 2u);
    const uint32 radius = ctx.params.value("radius", 32u);
    const float32 height = ctx.params.value("height", 20.0f);
    if (face >= PageStreaming::kPageStoreMaxFaces || x >= samples || z >= samples)
        return Editor::RefuseRequest("face must be 0 to 5 and x, z level-0 samples below the face's samples",
                                     json{{"faceSamples", samples}});
    if (radius == 0u || radius > kMaxBumpRadiusSamples)
        return Editor::RefuseRequest("radius must be 1 to 1024 level-0 samples");

    const std::vector<PageStreaming::PageStoreLevel> levels = PageStreaming::BuildPageStoreLevels(samples, samples);
    PageStreaming::PageSampleRect rect;
    rect.MinX = x > radius ? x - radius : 0u;
    rect.MinZ = z > radius ? z - radius : 0u;
    rect.MaxX = std::min(x + radius, samples - 1u);
    rect.MaxZ = std::min(z + radius, samples - 1u);
    service->WritePlanetPageLayer(entity.id, levels, static_cast<uint8>(face), rect,
                                  RaisedCosine(rect, x, z, radius, height));

    const Mathematics::Vector3 direction =
        PageStreaming::EqualAngleSampleDirection(static_cast<uint8>(face), x, z, samples);
    return json{{"ok", true},
                {"faceSamples", samples},
                {"texelMeters", texel},
                {"direction", {direction.x, direction.y, direction.z}},
                {"rect", {rect.MinX, rect.MinZ, rect.MaxX, rect.MaxZ}}};
}

} // namespace

void RegisterPlanetPageOverlayHandler(EditorDebugServer& server)
{
    // Params: face (0 to 5), x and z (level-0 samples of the face), radius (samples), height (meters);
    // writes the bump into the planet's layer over its rect; clear: true removes the layer. Replies
    // with the bump's unit direction from the planet's center.
    server.RegisterHandler("planet_page_overlay", PlacePlanetPageOverlay);
}

} // namespace GameEngine
