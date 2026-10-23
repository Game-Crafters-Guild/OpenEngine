#include <gtest/gtest.h>

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "SplineECS/SplineService.h"
#include "TerrainECS/TerrainColorMap.h"
#include "TerrainECS/TerrainColorMapBaker.h"
#include "TerrainECS/TerrainGrassField.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainColorMapCompose.h"
#include "Types/Color.h"

#include <algorithm>
#include <cmath>
#include <vector>

// The terrain colour map (TerrainColorMap.h): the compose of its writers on a lattice, the grass's
// ground write as the bake runs it, and the colour zones over it.

namespace GameEngine::TerrainECS
{
// The tiled terrains a baker holds state for: the ones its last update reached.
struct TerrainColorMapBakerTestAccess
{
    static std::size_t TrackedTiledTerrains(const TerrainColorMapBaker& baker) { return baker.m_Tiled.size(); }
};
} // namespace GameEngine::TerrainECS

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

// The soil the grass writes: TerrainGrass::SoilColor's default, linear (0.10, 0.075, 0.045).
ColorLinear DefaultSoil()
{
    const uint32 argb = Components::TerrainGrass{}.SoilColor;
    return ColorSRGB::FromSRGB255(static_cast<uint8>(argb >> 16), static_cast<uint8>(argb >> 8),
                                  static_cast<uint8>(argb))
        .ToLinear();
}

TerrainColorGroundWrite SoilWrite(float32 amount)
{
    TerrainColorGroundWrite write;
    write.Enabled = true;
    write.Amount = amount;
    const ColorLinear soil = DefaultSoil();
    write.SoilLinear[0] = soil.r;
    write.SoilLinear[1] = soil.g;
    write.SoilLinear[2] = soil.b;
    return write;
}

// A 3 x 3 lattice over [-1, 1]^2 whose splat is all grass (channel 0).
struct Lattice
{
    std::vector<uint8> Splat = std::vector<uint8>(3 * 3 * 4, 0);
    TerrainColorComposeInputs Inputs;

    Lattice()
    {
        for (size_t i = 0; i < Splat.size(); i += 4)
            Splat[i] = 255;
        Inputs.Width = Inputs.Height = 3;
        Inputs.OriginX = Inputs.OriginZ = -1.0f;
        Inputs.SizeX = Inputs.SizeZ = 2.0f;
        Inputs.Splat = {Splat.data(), 3, 3, true};
    }
};

const uint8* TexelAt(const TerrainColorMap& map, uint32 x, uint32 z)
{
    return map.Pixels.data() + (static_cast<size_t>(z) * map.Width + x) * kTerrainColorMapChannels;
}

void ExpectTexel(const TerrainColorMap& map, uint32 x, uint32 z, const float32 (&premultiplied)[3], float32 weight)
{
    ASSERT_TRUE(map.IsActive());
    const uint8* t = TexelAt(map, x, z);
    EXPECT_EQ(t[0], EncodeLinearToSrgbByte(premultiplied[0])) << x << "," << z;
    EXPECT_EQ(t[1], EncodeLinearToSrgbByte(premultiplied[1])) << x << "," << z;
    EXPECT_EQ(t[2], EncodeLinearToSrgbByte(premultiplied[2])) << x << "," << z;
    EXPECT_EQ(t[3], static_cast<uint8>(std::lround(weight * 255.0f))) << x << "," << z;
}

// A colour zone over [-1, 1]^2 whose 2 x 2 payload holds `left` on its left column and nothing on
// its right.
struct ZoneFixture
{
    std::vector<float32> Payload;
    ResolvedModifier Zone{};

    ZoneFixture(const float32 (&left)[3], float32 weight)
    {
        Payload.assign(2 * 2 * kZonePayloadColorChannels, 0.0f);
        for (uint32 z = 0; z < 2; ++z)
        {
            float32* t = &Payload[(static_cast<size_t>(z) * 2) * kZonePayloadColorChannels];
            t[0] = left[0];
            t[1] = left[1];
            t[2] = left[2];
            t[3] = weight;
        }
        Zone.ModType = ResolvedModifier::Type::ColorZone;
        Zone.Shape = Components::TerrainModifierShape::Rectangle;
        Zone.Enabled = true;
        Zone.Position = Mathematics::Vector3(0.0f, 0.0f, 0.0f);
        Zone.YawRadians = 0.0f;
        Zone.RectHalfX = Zone.RectHalfZ = 1.0f;
        Zone.Falloff = 0.0f;
        Zone.Priority = 1000.0f;
        Zone.ColorZone.Color = Payload.data();
        Zone.ColorZone.Width = 2;
        Zone.ColorZone.Height = 2;
        Zone.BoundsMinX = Zone.BoundsMinZ = -1.0f;
        Zone.BoundsMaxX = Zone.BoundsMaxZ = 1.0f;
    }
};

// A texel of a full field takes the soil at the authored amount, stored premultiplied: rgb the soil
// times the weight, a the weight.
TEST(TerrainColorMap, AFullFieldTexelTakesTheSoilAtTheAmount)
{
    Lattice lattice;
    lattice.Inputs.Write = SoilWrite(0.55f);
    TerrainColorMap map;
    ComposeTerrainColorMap(map, lattice.Inputs);
    const ColorLinear soil = DefaultSoil();
    const float32 expected[3] = {soil.r * 0.55f, soil.g * 0.55f, soil.b * 0.55f};
    for (uint32 z = 0; z < 3; ++z)
        for (uint32 x = 0; x < 3; ++x)
            ExpectTexel(map, x, z, expected, 0.55f);
}

// The weight follows the field coverage and the region scales: half the density writes half the
// weight, and a region's Darken Ground at 0 writes nothing inside it.
TEST(TerrainColorMap, TheWriteFollowsTheFieldCoverageAndTheRegionScale)
{
    Lattice lattice;
    lattice.Inputs.Write = SoilWrite(0.8f);
    TerrainGrassField grass;
    grass.Width = grass.Height = 3;
    grass.Pixels.assign(3 * 3 * kTerrainGrassFieldChannels, 255);
    grass.Pixels[(0 * 3 + 0) * kTerrainGrassFieldChannels + 1] = 128; // density 0.502 at (0, 0)
    grass.GroundDarkening.assign(3 * 3, 255);
    grass.GroundDarkening[2 * 3 + 2] = 0; // a region at 0% over (2, 2)
    lattice.Inputs.Grass = {&grass, -1.0f, -1.0f, 2.0f, 2.0f};
    TerrainColorMap map;
    ComposeTerrainColorMap(map, lattice.Inputs);
    const ColorLinear soil = DefaultSoil();
    const float32 halfWeight = 0.8f * (128.0f / 255.0f);
    const float32 half[3] = {soil.r * halfWeight, soil.g * halfWeight, soil.b * halfWeight};
    ExpectTexel(map, 0, 0, half, halfWeight);
    const float32 full[3] = {soil.r * 0.8f, soil.g * 0.8f, soil.b * 0.8f};
    ExpectTexel(map, 1, 1, full, 0.8f);
    const float32 none[3] = {0.0f, 0.0f, 0.0f};
    ExpectTexel(map, 2, 2, none, 0.0f);
}

// A colour zone composes over the ground write: at full weight its colour replaces the soil.
TEST(TerrainColorMap, AZoneComposesOverTheGroundWrite)
{
    Lattice lattice;
    lattice.Inputs.Write = SoilWrite(0.55f);
    const float32 red[3] = {1.0f, 0.0f, 0.0f};
    ZoneFixture zone(red, 1.0f);
    // Both payload columns written, so the whole zone is red at full weight.
    for (size_t i = 0; i < zone.Payload.size(); i += kZonePayloadColorChannels)
    {
        zone.Payload[i + 0] = 1.0f;
        zone.Payload[i + 3] = 1.0f;
    }
    lattice.Inputs.Zones = std::span<const ResolvedModifier>(&zone.Zone, 1);
    TerrainColorMap map;
    ComposeTerrainColorMap(map, lattice.Inputs);
    ExpectTexel(map, 1, 1, red, 1.0f);
}

// Between a written zone texel and an unwritten one the zone reads the half mix: its colour and its
// weight both halve, because the payload is filtered premultiplied. A straight colour would filter
// to half the colour at half the weight, a darker colour than either neighbour.
TEST(TerrainColorMap, TheMidpointOfAWrittenAndAnUnwrittenTexelIsTheHalfMix)
{
    Lattice lattice;
    const float32 color[3] = {0.6f, 0.3f, 0.1f};
    ZoneFixture zone(color, 1.0f);
    lattice.Inputs.Zones = std::span<const ResolvedModifier>(&zone.Zone, 1);
    TerrainColorMap map;
    ComposeTerrainColorMap(map, lattice.Inputs);
    // (1, 1) is world (0, 0): payload u = 0.5, halfway between the columns.
    const float32 half[3] = {0.3f, 0.15f, 0.05f};
    ExpectTexel(map, 1, 1, half, 0.5f);
}

// The composite needs no division: over a texel nothing has written, at any weight including 0, it
// is finite.
TEST(TerrainColorMap, ACompositeOverAnEmptyTexelIsFinite)
{
    for (const float32 weight : {0.0f, 1e-30f, 0.5f, 1.0f})
    {
        float32 texel[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float32 color[3] = {0.2f * weight, 0.4f * weight, 0.6f * weight};
        CompositePremultipliedOver(texel, color, weight);
        for (const float32 channel : texel)
            EXPECT_TRUE(std::isfinite(channel)) << weight;
        EXPECT_FLOAT_EQ(texel[3], weight);
    }
}

// No writer reaching the lattice leaves the map empty: identity everywhere, and no texture.
TEST(TerrainColorMap, NoWriterLeavesTheMapEmpty)
{
    Lattice lattice;
    TerrainColorMap map;
    ComposeTerrainColorMap(map, lattice.Inputs);
    EXPECT_FALSE(map.IsActive());
    lattice.Inputs.Write = SoilWrite(0.55f);
    ComposeTerrainColorMap(map, lattice.Inputs);
    ASSERT_TRUE(map.IsActive());
    lattice.Inputs.Write = SoilWrite(0.0f);
    ComposeTerrainColorMap(map, lattice.Inputs);
    EXPECT_FALSE(map.IsActive());
    EXPECT_TRUE(map.Dirty) << "an emptied map must retire its texture";
}

// ---- The write as the bake runs it ----------------------------------------------------------------

class TerrainColorMapBake : public testing::Test
{
protected:
    ECS::World World;
    TerrainModifierSystem System;
    TerrainData* Data = nullptr;
    ECS::EntityHandle TerrainEntity{};

    void SetUp() override
    {
        if (TerrainService::IsInitialized()) TerrainService::Shutdown();
        if (SplineECS::SplineService::IsInitialized()) SplineECS::SplineService::Shutdown();
        TerrainService::Initialize();
        SplineECS::SplineService::Initialize();
        EnableTerrainModifierLifecycleEvents(World);
        Terrain::TerrainConfig config{};
        config.HeightmapWidth = config.HeightmapHeight = 65;
        config.WorldSizeX = config.WorldSizeZ = 128;
        config.HeightScale = 32;
        config.LODLevels = 3;
        const TerrainHandle handle = TerrainService::Get().CreateTerrain(config);
        Data = TerrainService::Get().GetTerrainData(handle);
        ASSERT_NE(Data, nullptr);
        TerrainEntity = World.CreateEntity();
        Components::Terrain terrain{};
        terrain.SizeX = terrain.SizeZ = 128;
        terrain.HeightScale = 32;
        terrain.BaseSource = Components::TerrainBaseSource::Flat;
        terrain.TerrainDataHandle = handle.Index;
        terrain.TerrainDataGeneration = handle.Generation;
        World.AddComponentImmediate<Components::Terrain>(TerrainEntity, terrain);
        World.AddComponentImmediate<Components::WorldTransform>(TerrainEntity, {});
        // A ground modifier makes the bake run, so the terrain has a splat (all channel 0).
        const auto ground = World.CreateEntity();
        Components::TerrainModifierVolume volume{};
        volume.Shape = Components::TerrainVolumeShape::Global;
        World.AddComponentImmediate<Components::TerrainModifierVolume>(ground, volume);
        Components::TerrainFlattenEffect flatten{};
        flatten.UseVolumeHeight = false;
        World.AddComponentImmediate<Components::TerrainFlattenEffect>(ground, flatten);
        World.AddComponentImmediate<Components::WorldTransform>(ground, {});
    }

    void TearDown() override
    {
        World.Clear();
        SplineECS::SplineService::Shutdown();
        TerrainService::Shutdown();
    }

    void Tick()
    {
        World.SwapLifecycleEvents();
        System.Update(World, 1.0f / 60.0f);
    }

    void AddGrass(float32 amount)
    {
        Components::TerrainGrass grass{};
        grass.GroundDarkening = amount;
        World.AddComponentImmediate<Components::TerrainGrass>(TerrainEntity, grass);
    }

    uint8 AlphaAt(float32 x, float32 z) const
    {
        if (!Data->ColorMap.IsActive())
            return 0;
        const auto ix = static_cast<uint32>((x + 64) / 2);
        const auto iz = static_cast<uint32>((z + 64) / 2);
        return TexelAt(Data->ColorMap, ix, iz)[3];
    }
};

// The bake writes the soil where the field stands, a Grass Area region at Darken Ground 0% keeps it
// out of its shape, and raising the region back to 100% restores it.
TEST_F(TerrainColorMapBake, ARegionAtZeroClearsTheWriteAndOneHundredRestoresIt)
{
    AddGrass(0.55f);
    Tick();
    ASSERT_TRUE(Data->ColorMap.IsActive());
    EXPECT_EQ(AlphaAt(0, 0), 140);
    const auto region = World.CreateEntity();
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Rectangle;
    volume.RectHalfX = volume.RectHalfZ = 12;
    volume.Falloff = 2;
    World.AddComponentImmediate<Components::TerrainModifierVolume>(region, volume);
    Components::TerrainGrassEffect effect{};
    effect.GroundDarkeningScale = 0.0f;
    World.AddComponentImmediate<Components::TerrainGrassEffect>(region, effect);
    World.AddComponentImmediate<Components::WorldTransform>(region, {});
    Tick();
    EXPECT_EQ(AlphaAt(0, 0), 0) << "a region at 0% writes no soil inside it";
    EXPECT_EQ(AlphaAt(40, 40), 140) << "outside the region the soil stays";
    World.GetComponentForWrite<Components::TerrainGrassEffect>(region)->GroundDarkeningScale = 1.0f;
    Tick();
    EXPECT_EQ(AlphaAt(0, 0), 140);
}

// The map waits out an interactive edit (a drag held in the inspector or a gizmo): the value changes
// at every step, and the map recomposes once, after the edit is released.
TEST_F(TerrainColorMapBake, AHeldEditRecomposesTheMapOnceAfterItIsReleased)
{
    AddGrass(0.55f);
    Tick();
    ASSERT_EQ(AlphaAt(0, 0), 140);
    const uint64 composed = Data->ColorMap.Version;
    auto& service = TerrainService::Get();
    service.SetInteractiveModifierEdit(TerrainService::InteractiveEditSource::InspectorDrag, true);
    World.GetComponentForWrite<Components::TerrainGrass>(TerrainEntity)->GroundDarkening = 0.3f;
    Tick();
    Tick();
    EXPECT_EQ(Data->ColorMap.Version, composed) << "the map recomposed while the edit was held";
    EXPECT_EQ(AlphaAt(0, 0), 140);
    service.SetInteractiveModifierEdit(TerrainService::InteractiveEditSource::InspectorDrag, false);
    Tick();
    EXPECT_EQ(Data->ColorMap.Version, composed + 1) << "the release should recompose the map once";
    EXPECT_EQ(AlphaAt(0, 0), 77);
}

// Nothing writes the map for a terrain whose grass is disabled or places no blade, nor for one with
// Darken Ground at 0.
TEST_F(TerrainColorMapBake, ADisabledOrEmptyGrassWritesNothing)
{
    AddGrass(0.55f);
    Tick();
    ASSERT_TRUE(Data->ColorMap.IsActive());
    ECS::Entity(&World, TerrainEntity).SetEnabled<Components::TerrainGrass>(false);
    Tick();
    EXPECT_FALSE(Data->ColorMap.IsActive()) << "a disabled grass component writes no soil";
    ECS::Entity(&World, TerrainEntity).SetEnabled<Components::TerrainGrass>(true);
    World.GetComponentForWrite<Components::TerrainGrass>(TerrainEntity)->BladesPerSquareMeter = 0.0f;
    Tick();
    EXPECT_FALSE(Data->ColorMap.IsActive()) << "a row that places no blade writes no soil";
    auto* grass = World.GetComponentForWrite<Components::TerrainGrass>(TerrainEntity);
    grass->BladesPerSquareMeter = 21.0f;
    grass->GroundDarkening = 0.0f;
    Tick();
    EXPECT_FALSE(Data->ColorMap.IsActive()) << "Darken Ground 0 writes nothing";
}

// A planet carries no map, whatever its grass asks for.
TEST_F(TerrainColorMapBake, ASphereWritesNothing)
{
    AddGrass(0.55f);
    World.GetComponentForWrite<Components::Terrain>(TerrainEntity)->Domain = Components::TerrainDomain::Spherical;
    EXPECT_FALSE(TerrainColorMapBaker::ResolveGroundWrite(World, TerrainEntity).Enabled);
}

// A colour zone's payload reaches the map through the gather, composed over the soil.
TEST_F(TerrainColorMapBake, AColorZoneReachesTheMap)
{
    TerrainZonePayload payload;
    payload.Allocate(ZonePayloadFormat::ColorRGBA32F, 9, 9);
    for (size_t i = 0; i < payload.Color.size(); i += kZonePayloadColorChannels)
    {
        payload.Color[i + 0] = 1.0f;
        payload.Color[i + 3] = 1.0f;
    }
    const GUID guid = GUID::Generate();
    TerrainService::Get().SeedZonePayloadForTests(guid, std::move(payload));
    const auto zoneEntity = World.CreateEntity();
    Components::TerrainColorZone zone{};
    zone.ExtentX = zone.ExtentZ = 8.0f;
    zone.Falloff = 0.0f;
    zone.PayloadRef.Set(guid);
    World.AddComponentImmediate<Components::TerrainColorZone>(zoneEntity, zone);
    World.AddComponentImmediate<Components::WorldTransform>(zoneEntity, {});
    AddGrass(0.55f);
    Tick();
    EXPECT_EQ(AlphaAt(0, 0), 255);
    EXPECT_EQ(TexelAt(Data->ColorMap, 32, 32)[0], 255) << "the zone's red composes over the soil";
    EXPECT_EQ(AlphaAt(40, 40), 140) << "outside the zone the soil stays";
}

// A Color effect on a Grass Area's own volume colors the ground under that grass, over the soil, and
// only inside the volume; disabling it gives the region back to the soil.
TEST_F(TerrainColorMapBake, AColorEffectOnTheGrassVolumeWritesItsArea)
{
    AddGrass(0.55f);
    const auto region = World.CreateEntity();
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Rectangle;
    volume.RectHalfX = volume.RectHalfZ = 8;
    volume.Falloff = 0;
    World.AddComponentImmediate<Components::TerrainModifierVolume>(region, volume);
    World.AddComponentImmediate<Components::TerrainGrassEffect>(region, Components::TerrainGrassEffect{});
    Components::TerrainColorEffect color{};
    color.StackOrder = 1;
    color.Color[0] = 1.0f;
    color.Color[1] = color.Color[2] = 0.0f;
    color.Strength = 1.0f;
    World.AddComponentImmediate<Components::TerrainColorEffect>(region, color);
    World.AddComponentImmediate<Components::WorldTransform>(region, {});
    Tick();
    ASSERT_TRUE(Data->ColorMap.IsActive());
    EXPECT_EQ(AlphaAt(0, 0), 255);
    EXPECT_EQ(TexelAt(Data->ColorMap, 32, 32)[0], 255) << "the effect's red composes over the soil";
    EXPECT_EQ(TexelAt(Data->ColorMap, 32, 32)[1], 0);
    EXPECT_EQ(AlphaAt(40, 40), 140) << "outside the volume the soil stays";
    World.GetComponentForWrite<Components::TerrainColorEffect>(region)->Enabled = false;
    Tick();
    EXPECT_EQ(AlphaAt(0, 0), 140) << "a disabled Color effect leaves the soil";
}

// A volume with a Color effect and no Grass effect colors its area at Strength, with no grass field
// on the terrain, and a Strength edit recomposes the map without baking the ground.
TEST_F(TerrainColorMapBake, AColorAreaAloneWritesItsColorAtItsStrength)
{
    const auto region = World.CreateEntity();
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Rectangle;
    volume.RectHalfX = volume.RectHalfZ = 8;
    volume.Falloff = 0;
    World.AddComponentImmediate<Components::TerrainModifierVolume>(region, volume);
    Components::TerrainColorEffect color{};
    color.Color[0] = color.Color[1] = color.Color[2] = 1.0f;
    color.Strength = 0.5f;
    World.AddComponentImmediate<Components::TerrainColorEffect>(region, color);
    World.AddComponentImmediate<Components::WorldTransform>(region, {});
    Tick();
    ExpectTexel(Data->ColorMap, 32, 32, {0.5f, 0.5f, 0.5f}, 0.5f);
    EXPECT_EQ(AlphaAt(40, 40), 0) << "outside the volume nothing is written";
    const auto groundVersion = Data->HeightfieldVersion;
    World.GetComponentForWrite<Components::TerrainColorEffect>(region)->Strength = 1.0f;
    Tick();
    ExpectTexel(Data->ColorMap, 32, 32, {1.0f, 1.0f, 1.0f}, 1.0f);
    EXPECT_EQ(Data->HeightfieldVersion, groundVersion) << "a color edit does not bake the ground";
}

// A rectangle volume at the origin carrying a full-strength Color effect of one pure channel.
void AddColorRegion(ECS::World& world, float32 priority, uint32 channel)
{
    const auto region = world.CreateEntity();
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::Rectangle;
    volume.RectHalfX = volume.RectHalfZ = 8;
    volume.Falloff = 0;
    volume.Priority = priority;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(region, volume);
    Components::TerrainColorEffect color{};
    color.Color[0] = color.Color[1] = color.Color[2] = 0.0f;
    color.Color[channel] = 1.0f;
    color.Strength = 1.0f;
    world.AddComponentImmediate<Components::TerrainColorEffect>(region, color);
    world.AddComponentImmediate<Components::WorldTransform>(region, {});
}

// Where two Color effects overlap, the volume of higher priority composes last and shows.
TEST_F(TerrainColorMapBake, TheHigherPriorityColorEffectComposesLast)
{
    AddColorRegion(World, 1.0f, 0);
    AddColorRegion(World, 0.0f, 1);
    Tick();
    ExpectTexel(Data->ColorMap, 32, 32, {1.0f, 0.0f, 0.0f}, 1.0f);
}

// An atlas-backed terrain's coarse map is the write evaluated on the coarse splat, whose texels that
// never streamed read the grass layer: inside a resident tile it agrees with the tile's own map, so
// the frontier between the resident window and the coarse map shows no step, and past the window
// the soil continues.
TEST_F(TerrainColorMapBake, TheCoarseMapIsContinuousWithAResidentTilesMap)
{
    auto& service = TerrainService::Get();
    TiledTerrainConfig config{};
    config.WorldSizeX = 9216;
    config.WorldSizeZ = 1024;
    config.HeightScale = 32;
    config.SamplesPerMeter = 1;
    const auto handle = service.CreateTiledTerrain(config);
    auto* tiled = service.GetTiledTerrainData(handle);
    ASSERT_NE(tiled, nullptr);
    World.DestroyEntityImmediate(TerrainEntity);
    TerrainEntity = World.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = 9216;
    terrain.SizeZ = 1024;
    terrain.HeightScale = 32;
    terrain.TiledTerrainHandle = handle.Index;
    terrain.TiledTerrainGeneration = handle.Generation;
    World.AddComponentImmediate<Components::Terrain>(TerrainEntity, terrain);
    World.AddComponentImmediate<Components::WorldTransform>(TerrainEntity, {});
    AddGrass(0.55f);
    auto* tile = service.LoadTile(handle, {3, 0});
    ASSERT_NE(tile, nullptr);
    tile->LodState = TileLodState::Full;
    Tick();
    // The resident tile's splat moves from grass to a second layer across its width, so the field's
    // coverage, and the soil with it, falls off across the tile.
    ASSERT_GE(tile->SplatmapWidth, 2u);
    for (uint32 z = 0; z < tile->SplatmapHeight; ++z)
        for (uint32 x = 0; x < tile->SplatmapWidth; ++x)
        {
            uint8* texel = &tile->Splatmap[(static_cast<size_t>(z) * tile->SplatmapWidth + x) * 4];
            const uint8 second = static_cast<uint8>(255u * x / (tile->SplatmapWidth - 1));
            texel[0] = static_cast<uint8>(255u - second);
            texel[1] = second;
            texel[2] = texel[3] = 0;
        }
    tile->MarkSplatChanged();
    Tick();
    ASSERT_TRUE(tile->ColorMap.IsActive()) << "the resident tile composed no map";
    const TerrainColorMap& coarse = tiled->ColorCoarseMap;
    ASSERT_TRUE(coarse.IsActive());

    const float32 ox = -4608.0f, oz = -512.0f;
    const float32 tileSize = tiled->Config.TileWorldSize;
    const float32 tileX = ox + 3.0f * tileSize;
    const TerrainColorMap& own = tile->ColorMap;
    const auto ownAlphaAt = [&](float32 x, float32 z) {
        const float32 fx = std::clamp((x - tileX) / tileSize, 0.0f, 1.0f) * static_cast<float32>(own.Width - 1);
        const float32 fz = std::clamp((z - oz) / tileSize, 0.0f, 1.0f) * static_cast<float32>(own.Height - 1);
        const uint32 x0 = static_cast<uint32>(fx), z0 = static_cast<uint32>(fz);
        const uint32 x1 = std::min(x0 + 1, own.Width - 1), z1 = std::min(z0 + 1, own.Height - 1);
        const auto a = [&](uint32 ix, uint32 iz) { return static_cast<float32>(TexelAt(own, ix, iz)[3]); };
        const float32 tx = fx - static_cast<float32>(x0), tz = fz - static_cast<float32>(z0);
        const float32 top = a(x0, z0) + (a(x1, z0) - a(x0, z0)) * tx;
        const float32 bottom = a(x0, z1) + (a(x1, z1) - a(x0, z1)) * tx;
        return top + (bottom - top) * tz;
    };
    uint32 compared = 0, varied = 0;
    for (uint32 j = 0; j < coarse.Height; ++j)
        for (uint32 i = 0; i < coarse.Width; ++i)
        {
            const float32 x = ox + 9216.0f * static_cast<float32>(i) / static_cast<float32>(coarse.Width - 1);
            const float32 z = oz + 1024.0f * static_cast<float32>(j) / static_cast<float32>(coarse.Height - 1);
            if (x < tileX || x > tileX + tileSize)
                continue;
            const float32 coarseAlpha = TexelAt(coarse, i, j)[3];
            const float32 ownAlpha = ownAlphaAt(x, z);
            EXPECT_NEAR(coarseAlpha, ownAlpha, 6.0f) << "coarse texel " << i << "," << j << " at x " << x;
            ++compared;
            varied += ownAlpha < 135.0f ? 1u : 0u;
        }
    EXPECT_GT(compared, 100u);
    EXPECT_GT(varied, 5u) << "the paint left the soil uniform, so the comparison proves nothing";
    // Past the resident window, where nothing streamed, the soil continues at the full amount.
    EXPECT_EQ(TexelAt(coarse, coarse.Width - 1, coarse.Height / 2)[3], 140);
}

// A paint stroke records the splat region it wrote (MarkSplatChanged with a rect); the map follows
// that region on the next update, with no full rebake.
TEST_F(TerrainColorMapBake, ASplatRegionEditRecomposesTheMap)
{
    AddGrass(0.55f);
    Tick();
    ASSERT_TRUE(Data->ColorMap.IsActive());
    ASSERT_EQ(AlphaAt(0, 0), 140);
    const uint32 ix = 32, iz = 32;
    uint8* splat = Data->Splatmap.data() + (static_cast<size_t>(iz) * Data->SplatmapWidth + ix) * 4u;
    splat[0] = 0;
    splat[1] = 255;
    splat[2] = 0;
    splat[3] = 0;
    Data->MarkSplatChanged({static_cast<int32>(ix), static_cast<int32>(iz), static_cast<int32>(ix + 1),
                            static_cast<int32>(iz + 1)});
    System.Update(World, 1.0f / 60.0f);
    EXPECT_EQ(AlphaAt(0, 0), 0) << "the soil stays on a texel whose splat left layer 0";
}

// The baker keeps a little state per tiled terrain; a terrain that goes away takes its state with it.
TEST_F(TerrainColorMapBake, ATiledTerrainThatGoesAwayLeavesNoState)
{
    auto& service = TerrainService::Get();
    TiledTerrainConfig config{};
    config.WorldSizeX = 2048;
    config.WorldSizeZ = 1024;
    config.HeightScale = 32;
    config.SamplesPerMeter = 1;
    const auto handle = service.CreateTiledTerrain(config);
    const auto entity = World.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = 2048;
    terrain.SizeZ = 1024;
    terrain.HeightScale = 32;
    terrain.TiledTerrainHandle = handle.Index;
    terrain.TiledTerrainGeneration = handle.Generation;
    World.AddComponentImmediate<Components::Terrain>(entity, terrain);
    World.AddComponentImmediate<Components::WorldTransform>(entity, {});
    TerrainColorMapBaker baker;
    baker.Update(World, service, {}, 0);
    EXPECT_EQ(TerrainColorMapBakerTestAccess::TrackedTiledTerrains(baker), 1u);
    World.DestroyEntityImmediate(entity);
    baker.Update(World, service, {}, 0);
    EXPECT_EQ(TerrainColorMapBakerTestAccess::TrackedTiledTerrains(baker), 0u);
}

} // namespace
