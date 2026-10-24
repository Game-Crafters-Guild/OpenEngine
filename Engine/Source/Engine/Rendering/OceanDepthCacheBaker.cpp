#include "Engine/Rendering/OceanDepthCacheBaker.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "TerrainECS/TerrainService.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <utility>
#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace
{

void SetError(std::string* error, const char* message)
{
    if (error)
        *error = message;
}

void MultiplyMatrices(const float32 a[16], const float32 b[16], float32 out[16])
{
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            out[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

struct SceneMeshCandidate
{
    GUID ModelGuid;
    uint32 MeshId = 0;
    Rendering::RenderLayerMask RenderLayers;
    bool IsSkinnedRenderer = false;
    float32 WorldTransform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
};

void CollectSceneMeshCandidates(ECS::World& world,
                                const OceanSceneDepthBakeOptions& options,
                                std::vector<SceneMeshCandidate>& outCandidates,
                                OceanSceneDepthBakeStats& stats)
{
    auto query = world.Query<ECS::Read<Components::MeshRenderer>,
                             ECS::Read<Components::WorldTransform>,
                             ECS::Optional<Components::OceanMeshDepthContributor>,
                             ECS::Optional<ECS::ComponentDisabled<Components::OceanMeshDepthContributor>>>();
    // The bake option decides whether disabled renderers contribute at all, so
    // it decides whether the query visits them.
    if (options.IncludeDisabledRenderers)
        query.IncludeDisabled();
    query.Each([&](ECS::EntityHandle entity,
                   const Components::MeshRenderer& renderer,
                   const Components::WorldTransform& worldTransform,
                   const Components::OceanMeshDepthContributor* depthContributor,
                   const ECS::ComponentDisabled<Components::OceanMeshDepthContributor>* contributorOff)
        {
            if (options.OnlyOceanMeshDepthContributors)
            {
                // The opt-in above reaches disabled renderers, never a switched-off tag.
                if (!depthContributor || contributorOff)
                {
                    ++stats.SkippedUntagged;
                    return;
                }
                if (!renderer.RenderLayers.Intersects(depthContributor->ContributorLayers))
                {
                    ++stats.SkippedLayer;
                    return;
                }
            }
            if (!renderer.RenderLayers.Intersects(options.Layers))
            {
                ++stats.SkippedLayer;
                return;
            }
            if (renderer.modelAssetGuid.IsNull())
            {
                ++stats.SkippedAsset;
                return;
            }

            const bool isSkinned = world.GetComponent<Components::SkinnedMeshRenderer>(entity) != nullptr;
            if (isSkinned && !options.IncludeSkinnedMeshes)
            {
                ++stats.SkippedSkinned;
                return;
            }

            SceneMeshCandidate candidate{};
            candidate.ModelGuid = renderer.modelAssetGuid.ToGuid();
            candidate.MeshId = renderer.meshId;
            candidate.RenderLayers = renderer.RenderLayers;
            candidate.IsSkinnedRenderer = isSkinned;
            std::memcpy(candidate.WorldTransform, worldTransform.matrix, sizeof(candidate.WorldTransform));
            outCandidates.push_back(candidate);
            ++stats.CandidateRenderers;
        });
}

// A planar terrain the bake reads through its height query.
struct TerrainBakeSource
{
    Components::Terrain Terrain;
    TerrainECS::SurfaceHeightQuery Query;
};

// The terrains of one bake and the texels they cover but cannot answer.
struct TerrainBakeContext
{
    TerrainECS::TerrainService* Service = nullptr;
    std::vector<TerrainBakeSource> Terrains;
    uint32 UnsampledTexels = 0;
};

void CollectSceneTerrainSources(ECS::World& world,
                                const OceanSceneDepthBakeOptions& options,
                                TerrainBakeContext& outContext,
                                OceanSceneDepthBakeStats& stats)
{
    if (!options.IncludeTerrainHeightfields)
        return;

    outContext.Service = TerrainECS::TerrainService::TryGet();

    auto query = world.Query<ECS::Read<Components::Terrain>,
                             ECS::Read<Components::WorldTransform>>();
    if (options.IncludeDisabledRenderers)
        query.IncludeDisabled();
    query.Each([&](ECS::EntityHandle,
                   const Components::Terrain& terrain,
                   const Components::WorldTransform& worldTransform)
        {
            if (!outContext.Service || terrain.Domain == Components::TerrainDomain::Spherical)
            {
                ++stats.SkippedTerrain;
                return;
            }
            if (!terrain.RenderLayers.Intersects(options.Layers))
            {
                ++stats.SkippedLayer;
                return;
            }
            TerrainECS::SurfaceHeightQuery surface = outContext.Service->SurfaceQuery(terrain, worldTransform);
            if (!surface.IsValid())
            {
                ++stats.SkippedTerrain;
                return;
            }
            outContext.Terrains.push_back(TerrainBakeSource{terrain, surface});
            ++stats.TerrainSources;
        });
}

// The highest finest terrain floor at world (x, z). A terrain whose finest page there is not on the
// CPU fetches it now: the bake is one call, so no height-page update trims the pages it fetched
// until it returns, and the pages under its area are its reader window. A texel a terrain covers
// but cannot answer (no page source: its height store is still cooking) is counted unsampled.
bool SampleTerrainFloors(float32 worldX, float32 worldZ, float32& outFloorY, void* userData)
{
    auto* context = static_cast<TerrainBakeContext*>(userData);
    const Mathematics::Vector3 point(worldX, 0.0f, worldZ);
    bool found = false;
    bool unsampled = false;
    float32 highestFloor = std::numeric_limits<float32>::lowest();
    for (TerrainBakeSource& source : context->Terrains)
    {
        if (!source.Query.ContainsXZ(worldX, worldZ))
            continue;
        TerrainECS::SurfaceHeightSample sample;
        constexpr TerrainECS::SurfaceHeightPolicy kFinest = TerrainECS::SurfaceHeightPolicy::RequireFinest;
        const bool sampled = source.Query.SampleHeight(point, kFinest, sample) ||
                             (context->Service->FetchFinestHeightPage(source.Terrain, point) &&
                              source.Query.SampleHeight(point, kFinest, sample));
        if (!sampled)
        {
            unsampled = true;
            continue;
        }
        highestFloor = found ? std::max(highestFloor, sample.Height) : sample.Height;
        found = true;
    }
    context->UnsampledTexels += unsampled ? 1u : 0u;
    if (!found)
        return false;
    outFloorY = highestFloor;
    return true;
}

SharedPtr<Asset> GetOrLoadAsset(AssetManager& assetManager,
                                const GUID& guid,
                                bool loadMissing)
{
    SharedPtr<Asset> asset = assetManager.GetAsset(guid);
    if ((!asset || !asset->IsLoaded()) && loadMissing)
        asset = assetManager.LoadAssetAsync(guid).get();
    return asset;
}

} // anonymous namespace

bool BakeOceanDepthCacheFromSceneMeshes(ECS::World& world,
                                        const Ocean::OceanDepthCacheBakeDesc& desc,
                                        const OceanSceneDepthBakeOptions& options,
                                        Ocean::OceanDepthCacheAsset& outCache,
                                        OceanSceneDepthBakeStats* outStats,
                                        std::string* error)
{
    OceanSceneDepthBakeStats stats{};
    std::vector<SceneMeshCandidate> candidates;
    CollectSceneMeshCandidates(world, options, candidates, stats);
    TerrainBakeContext terrains;
    CollectSceneTerrainSources(world, options, terrains, stats);

    std::vector<Ocean::OceanDepthCacheMeshSource> sources;
    sources.reserve(candidates.size());
    std::vector<SharedPtr<Asset>> heldAssets;
    heldAssets.reserve(candidates.size());

    AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
    for (const SceneMeshCandidate& candidate : candidates)
    {
        SharedPtr<Asset> asset = GetOrLoadAsset(assetManager, candidate.ModelGuid, options.LoadMissingAssets);
        if (!asset || !asset->IsLoaded() || asset->GetType() != AssetType::Model)
        {
            ++stats.SkippedAsset;
            continue;
        }

        auto* model = static_cast<ModelAsset*>(asset.get());
        if (candidate.MeshId >= model->GetMeshCount())
        {
            ++stats.SkippedMesh;
            continue;
        }

        const Mesh& mesh = model->GetMesh(candidate.MeshId);
        if (mesh.PrimitiveTopology != MeshPrimitiveTopology::Triangles ||
            mesh.Vertices.empty() ||
            mesh.Indices.size() < 3)
        {
            ++stats.SkippedMesh;
            continue;
        }
        if ((candidate.IsSkinnedRenderer || mesh.IsSkinned()) && !options.IncludeSkinnedMeshes)
        {
            ++stats.SkippedSkinned;
            continue;
        }

        Ocean::OceanDepthCacheMeshSource source{};
        source.Positions = reinterpret_cast<const Mathematics::Vector3*>(mesh.Vertices.data());
        source.VertexStride = static_cast<uint32>(sizeof(Vertex));
        source.VertexCount = static_cast<uint32>(mesh.Vertices.size());
        source.Indices = mesh.Indices.data();
        source.IndexCount = static_cast<uint32>(mesh.Indices.size());
        source.UseLocalToWorld = true;
        source.Layers = candidate.RenderLayers;
        MultiplyMatrices(candidate.WorldTransform, mesh.SourceNodeTransform, source.LocalToWorld);

        sources.push_back(source);
        heldAssets.push_back(std::move(asset));
        ++stats.MeshSources;
        stats.Triangles += static_cast<uint32>(mesh.Indices.size() / 3u);
    }

    std::vector<Ocean::OceanDepthCacheAsset> partialCaches;
    partialCaches.reserve(2);

    if (!sources.empty())
    {
        Ocean::OceanDepthCacheAsset meshCache;
        if (!Ocean::BakeOceanDepthCacheFromMeshes(desc,
                                                  sources.data(),
                                                  static_cast<uint32>(sources.size()),
                                                  options.Layers,
                                                  meshCache,
                                                  error))
        {
            if (outStats)
                *outStats = stats;
            return false;
        }
        partialCaches.push_back(std::move(meshCache));
    }

    if (!terrains.Terrains.empty())
    {
        Ocean::OceanDepthCacheAsset terrainCache;
        const bool baked = Ocean::BakeOceanDepthCache(desc, &SampleTerrainFloors, &terrains, terrainCache, error);
        stats.UnsampledTerrainTexels = terrains.UnsampledTexels;
        if (baked && terrains.UnsampledTexels != 0u && error)
            *error = std::format("{} texels lie on a terrain with no height yet (its height store is still cooking). "
                                 "Bake again when the terrain has finished loading.",
                                 terrains.UnsampledTexels);
        if (!baked || terrains.UnsampledTexels != 0u)
        {
            if (outStats)
                *outStats = stats;
            return false;
        }
        partialCaches.push_back(std::move(terrainCache));
    }

    if (outStats)
        *outStats = stats;

    if (partialCaches.empty())
    {
        SetError(error, "scene ocean depth bake found no usable mesh or terrain sources");
        return false;
    }

    if (partialCaches.size() == 1)
    {
        outCache = std::move(partialCaches.front());
        return true;
    }

    return Ocean::ComposeOceanDepthCaches(
        partialCaches.data(), static_cast<uint32>(partialCaches.size()), outCache, error);
}

bool SaveOceanDepthCacheFromSceneMeshes(ECS::World& world,
                                        const Ocean::OceanDepthCacheBakeDesc& desc,
                                        const OceanSceneDepthBakeOptions& options,
                                        const std::filesystem::path& path,
                                        OceanSceneDepthBakeStats* outStats,
                                        std::string* error)
{
    Ocean::OceanDepthCacheAsset cache;
    if (!BakeOceanDepthCacheFromSceneMeshes(world, desc, options, cache, outStats, error))
        return false;
    return cache.SaveBinary(path, error);
}

} // namespace GameEngine::Engine::Renderer
