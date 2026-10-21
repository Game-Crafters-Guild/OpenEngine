#pragma once

#include "AssetCore/GUID.h"
#include "Engine/Rendering/ParallaxReliefDepth.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine::Engine::Renderer
{
class Material;
class RenderServices;

/// One colour variant a world pass draws a scene mesh with: the layout the mesh binds and the
/// keywords of the draw (WorldEntityColorKeywords, plus LodCrossfade for the tail of a mesh with
/// an LOD chain).
struct SceneColorDrawKey
{
    Rendering::VertexAttributeFlags VertexFlags = Rendering::VertexAttributeFlags::None;
    Rendering::MaterialKeyword PassKeywords = Rendering::MaterialKeyword::None;

    bool operator==(const SceneColorDrawKey&) const = default;
};

/// The colour variants a world pass whose keywords are `worldPassKeywords` and whose relief depth
/// comes from `relief` draws `material` with on a mesh that binds `boundVertexFlags` and carries
/// `lodCount` levels: the head draw, and the crossfading tail when the mesh has more than one level
/// (the pass adds LodCrossfade per tail segment). The same keys the world pass's entity draws look
/// their variants up by.
std::vector<SceneColorDrawKey> EnumerateSceneColorDrawKeys(const Material& material,
                                                           Rendering::VertexAttributeFlags boundVertexFlags,
                                                           uint32_t lodCount,
                                                           Rendering::MaterialKeyword worldPassKeywords,
                                                           ReliefDepthSource relief);

/// The scene warm, one per material system (MaterialSystem::SceneWarm): every scene resolve (the
/// runtime's and the editor's scene-open build) notes each (mesh, material) its entities draw as the
/// models land, and each frame begin steps it. A step requests, for each noted draw whose material
/// has published its base pipeline, the colour variants every world view draws it with
/// (EnumerateSceneColorDrawKeys): the variant compiles on the compile workers and the device then
/// builds its pipeline for the view's world-pass formats on a worker
/// (PipelineVariantCache::WarmColorVariant). No frame waits on it. A view's keywords and formats are
/// known once it has declared a world pass, so a draw noted before the first frame is requested at
/// the next frame begin. The camera prepass and the shadow passes are not warmed here: their opaque
/// draws share one depth pipeline per layout, and a material that needs its own resolves it at its
/// first draw.
class SceneDrawWarm
{
public:
    /// Notes that `mesh` draws with the material `materialGuid` in the scene. A pair noted again
    /// while it waits is requested once; noted again after its request (a scene opened again), it
    /// is requested again, which finds its variant cached.
    void Note(Rendering::MeshGPUHandle mesh, const GUID& materialGuid);

    /// Requests the colour variants of every noted draw whose material is registered with a
    /// published base pipeline, for every view that has declared a world pass, and forgets those
    /// draws and the draws whose mesh is no longer registered. Returns how many draws it requested.
    /// Serial frame-begin thread.
    size_t Step(RenderServices& renderServices);

    /// Noted draws not requested yet.
    size_t PendingCount() const { return m_Pending.size(); }

private:
    struct NotedDraw
    {
        Rendering::MeshGPUHandle Mesh{};
        GUID Material;
    };
    // What a view's last world pass declared: the keywords, the relief depth source and the formats.
    struct WorldView
    {
        Rendering::MaterialKeyword Keywords = Rendering::MaterialKeyword::None;
        ReliefDepthSource Relief = ReliefDepthSource::NoPrepass;
        Rendering::PipelineFormatKey Formats{};
    };

    // Requests `draw`'s colour variants for every one of `views`. True when the draw is done with:
    // requested, or its mesh is no longer registered. False (to ask again next step) while its
    // mesh is not drawable yet or its material is unregistered or still building its base
    // pipeline, whose state its variants copy.
    static bool WarmDraw(const NotedDraw& draw, RenderServices& renderServices, const std::vector<WorldView>& views);
    struct NotedDrawHash
    {
        size_t operator()(const std::pair<uint64_t, GUID>& draw) const noexcept;
    };

    std::vector<NotedDraw> m_Pending;
    // The pairs in m_Pending, so a pair noted again while it waits is not queued twice.
    std::unordered_set<std::pair<uint64_t, GUID>, NotedDrawHash> m_Noted;
};
} // namespace GameEngine::Engine::Renderer
