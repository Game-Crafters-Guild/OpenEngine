#include "TerrainTestHelpers.h"

#include "AssetCore/GUID.h"
#include "Components/Terrain/Terrain.h"
#include "Mathematics/Vector3.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageStore/PageStoreFormat.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/TerrainService.h"

#include <initializer_list>

namespace GameEngine::TerrainTests
{

namespace
{
const GUID& NoiseBaseHeightmapGuid()
{
    static const GUID guid = GUID::Generate();
    return guid;
}
} // namespace

void SeedNoiseBaseHeightmap(TerrainECS::TerrainService& service, Terrain::HeightfieldData& heightfield)
{
    heightfield.FillRegionWithNoise(4.0f, 1.0f, 0, 0, static_cast<int32>(heightfield.GetWidth()) - 1,
                                    static_cast<int32>(heightfield.GetHeight()) - 1, 5, 42);
    service.SeedDecodedHeightmapForTests(NoiseBaseHeightmapGuid(), heightfield);
}

void UseNoiseBaseHeightmap(Components::Terrain& terrain)
{
    terrain.BaseSource = Components::TerrainBaseSource::HeightmapAsset;
    terrain.TerrainAssetGuid.Set(NoiseBaseHeightmapGuid());
}

TerrainECS::TiledTerrainBase GeneratedNoiseBase()
{
    TerrainECS::TiledTerrainBase base;
    base.Noise = Terrain::TerrainNoiseStack{};
    return base;
}

float32 PlanetLayerHeight(const TerrainECS::TerrainService& service, uint32 planet,
                          const Mathematics::Vector3& direction)
{
    const TerrainECS::TerrainService::PlanetPageOverlay* overlay = service.FindPlanetPageOverlay(planet);
    float32 height = 0.0f;
    for (const PageStreaming::HeightPageLayer* layer :
         {overlay ? overlay->Modifiers.get() : nullptr, overlay ? overlay->Layer.get() : nullptr})
    {
        if (!layer)
            continue;
        const PageStreaming::PageFacePoint point =
            PageStreaming::EqualAngleFacePointOf(direction, layer->Levels()[0].SamplesX);
        height += layer->SampleLevel0(point.Face, point.X, point.Z);
    }
    return height;
}

} // namespace GameEngine::TerrainTests
