#include "TerrainECS/TerrainEntityProvisioning.h"

#include "Components/Name.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Transform.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainDefaultSurfaceRules.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainSizingPlan.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace GameEngine::TerrainECS
{

namespace
{

// A Shape::Global volume covers every terrain in the scene at weight 1, so the default rows
// belong to the SCENE, not to one terrain: a second copy would composite on top of the first
// rather than scoping itself to the new terrain.
bool SceneHasGlobalSurfaceRules(ECS::World& world)
{
    bool found = false;
    world.Query<ECS::Read<Components::TerrainModifierVolume>,
                ECS::Read<Components::TerrainSurfaceRulesEffect>>()
        .Each([&](const Components::TerrainModifierVolume& volume,
                  const Components::TerrainSurfaceRulesEffect&) {
            found = found || volume.Shape == Components::TerrainVolumeShape::Global;
        });
    return found;
}

// An ordinary entity named `label` with an identity Transform and a Shape::Global volume.
ECS::EntityHandle SpawnGlobalVolume(ECS::World& world, std::string_view label)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    if (!entity.IsValid())
        return entity;

    // Bounded copy with an explicit terminator: Name is a fixed char buffer.
    Components::Name name{};
    const std::size_t copied = std::min(sizeof(name.value) - 1u, label.size());
    std::memcpy(name.value, label.data(), copied);
    name.value[copied] = '\0';
    world.AddComponentImmediate(entity, name);
    world.AddComponentImmediate(entity, Components::Transform{});

    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Global;
    world.AddComponentImmediate(entity, volume);
    return entity;
}

// The default surface rules, as an ORDINARY entity. Nothing marks it, nothing re-creates it once
// deleted, and it is indistinguishable in the scene file from one authored by hand. It appears
// when a terrain is created and never on load, so deleting it is permanent for that scene.
ECS::EntityHandle SpawnDefaultSurfaceRulesVolume(ECS::World& world, const Components::Terrain& config)
{
    // Read before the spawn: the query walks the scene's volumes.
    const std::optional<GeneratedNoiseRelief> generatedNoise = ResolveGeneratedNoiseRelief(world);
    const ECS::EntityHandle entity = SpawnGlobalVolume(world, "Terrain Surface Rules");
    if (!entity.IsValid())
        return entity;

    // Without the service the terrain has no data yet: its range is the nominal band.
    TerrainHeightRangeMeters range{0.0f, config.HeightScale};
    if (auto* terrainService = TerrainService::TryGet())
        range = terrainService->GetHeightRangeMeters(config, generatedNoise);
    world.AddComponentImmediate(entity, MakeDefaultTerrainSurfaceRules(range.Min, range.Max));
    return entity;
}

} // namespace

TerrainProvisioningResult ProvisionTerrainEntity(ECS::World& world, ECS::EntityHandle entity,
                                                 const Components::Terrain& config,
                                                 const Components::TerrainPlanetRelief& relief)
{
    TerrainProvisioningResult result;
    const bool spherical = config.Domain == Components::TerrainDomain::Spherical;
    if (!spherical)
    {
        result.Error = PlanarTerrainSizeError(config.SizeX, config.SizeZ, config.SamplesPerMeter);
        if (!result.Error.empty())
            return result;
    }
    if (!world.GetComponent<Components::Terrain>(entity))
        world.AddComponentImmediate(entity, config);
    else
        *world.GetComponentForWrite<Components::Terrain>(entity) = config;

    if (!world.GetComponent<Components::TerrainGrass>(entity))
        world.AddComponentImmediate(entity, Components::TerrainGrass{});

    // A planet's base relief is a companion component; a planar terrain carries none.
    // Re-provisioning an existing spherical entity refreshes it to the passed values.
    if (spherical)
        world.AddComponentImmediate(entity, relief);
    else if (world.GetComponent<Components::TerrainPlanetRelief>(entity))
        world.RemoveComponentImmediate<Components::TerrainPlanetRelief>(entity);

    // A planar terrain is tiled: the extraction system provisions its tiles, pages and page colliders
    // on the next tick. A planet below the per-tile cap gets its data now, so it renders at once.
    if (spherical && !Terrain::TerrainNeedsTiling(config.SizeX, config.SizeZ, config.SamplesPerMeter))
    {
        if (auto* terrainService = TerrainService::TryGet())
        {
            const auto terrainConfig = Terrain::TerrainConfig::FromSamplesPerMeter(
                config.SizeX, config.SizeZ, config.HeightScale, config.SamplesPerMeter);
            const TerrainHandle terrainHandle = terrainService->CreateTerrainFromBase(terrainConfig, config);
            if (auto* comp = world.GetComponentForWrite<Components::Terrain>(entity))
            {
                comp->TerrainDataHandle = terrainHandle.Index;
                comp->TerrainDataGeneration = terrainHandle.Generation;
            }
        }
    }

    // Rules evaluate on the planar bake only, so a planet gets none: an invisible modifier that
    // cannot fire would be worse than no modifier.
    if (!spherical && !SceneHasGlobalSurfaceRules(world))
        result.SurfaceRulesVolume = SpawnDefaultSurfaceRulesVolume(world, config);

    return result;
}

uint32 PlaceProvisionedSurfaceRules(ECS::World& world)
{
    auto* terrainService = TerrainService::TryGet();
    if (!terrainService)
        return 0;

    // Once every rule set is the author's, the tick reads no terrain range and builds no rules.
    bool anyMarked = false;
    world.Query<ECS::Read<Components::TerrainSurfaceRulesEffect>>().Each(
        [&](const Components::TerrainSurfaceRulesEffect& effect) { anyMarked = anyMarked || effect.ProvisionedDefaults; });
    if (!anyMarked)
        return 0;

    bool anyPlanar = false;
    const std::optional<GeneratedNoiseRelief> generatedNoise = ResolveGeneratedNoiseRelief(world);
    TerrainHeightRangeMeters joined{std::numeric_limits<float32>::max(), std::numeric_limits<float32>::lowest()};
    world.Query<ECS::Read<Components::Terrain>>().Each([&](const Components::Terrain& terrain) {
        if (terrain.Domain != Components::TerrainDomain::Planar)
            return;
        const TerrainHeightRangeMeters range = terrainService->GetHeightRangeMeters(terrain, generatedNoise);
        joined.Min = std::min(joined.Min, range.Min);
        joined.Max = std::max(joined.Max, range.Max);
        anyPlanar = true;
    });
    if (!anyPlanar)
        return 0;

    const Components::TerrainSurfaceRulesEffect placed = MakeDefaultTerrainSurfaceRules(joined.Min, joined.Max);
    std::vector<ECS::EntityHandle> stale;
    world.Query<ECS::Read<Components::TerrainSurfaceRulesEffect>>().Each(
        [&](ECS::EntityHandle entity, const Components::TerrainSurfaceRulesEffect& effect) {
            if (effect.ProvisionedDefaults && std::memcmp(effect.Rules, placed.Rules, sizeof(placed.Rules)) != 0)
                stale.push_back(entity);
        });
    for (const ECS::EntityHandle entity : stale)
    {
        auto* effect = world.GetComponentForWrite<Components::TerrainSurfaceRulesEffect>(entity);
        effect->RuleCount = placed.RuleCount;
        std::memcpy(effect->Rules, placed.Rules, sizeof(placed.Rules));
    }
    return static_cast<uint32>(stale.size());
}

ECS::EntityHandle SpawnGeneratedNoiseVolume(ECS::World& world)
{
    const ECS::EntityHandle entity = SpawnGlobalVolume(world, "Terrain Noise");
    if (!entity.IsValid())
        return entity;
    Components::TerrainNoiseEffect noise{};
    noise.Mode = Components::TerrainNoiseMode::Generated;
    noise.Preset = Components::TerrainNoisePreset::Rolling;
    world.AddComponentImmediate(entity, noise);
    return entity;
}

} // namespace GameEngine::TerrainECS
