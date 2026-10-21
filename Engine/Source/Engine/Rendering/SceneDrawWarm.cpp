#include "SceneDrawWarm.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewRegistry.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{
std::vector<SceneColorDrawKey> EnumerateSceneColorDrawKeys(const Material& material,
                                                           Rendering::VertexAttributeFlags boundVertexFlags,
                                                           uint32_t lodCount,
                                                           Rendering::MaterialKeyword worldPassKeywords,
                                                           ReliefDepthSource relief)
{
    const Rendering::MaterialKeyword head = WorldEntityColorKeywords(
        worldPassKeywords, material.GetVariantKey().materialKeywords, material.GetAlphaMode(), relief);
    std::vector<SceneColorDrawKey> keys{{boundVertexFlags, head}};
    if (lodCount > 1)
        keys.push_back({boundVertexFlags, head | Rendering::MaterialKeyword::LodCrossfade});
    return keys;
}

size_t SceneDrawWarm::NotedDrawHash::operator()(const std::pair<uint64_t, GUID>& draw) const noexcept
{
    const size_t h = std::hash<uint64_t>{}(draw.first);
    return h ^ (std::hash<GUID>{}(draw.second) + 0x9e3779b9 + (h << 6) + (h >> 2));
}

void SceneDrawWarm::Note(Rendering::MeshGPUHandle mesh, const GUID& materialGuid)
{
    if (!mesh.IsValid() || materialGuid.IsNull())
        return;
    if (m_Noted.insert({static_cast<uint64_t>(mesh), materialGuid}).second)
        m_Pending.push_back({mesh, materialGuid});
}

size_t SceneDrawWarm::Step(RenderServices& renderServices)
{
    if (m_Pending.empty())
        return 0;
    // A view that has declared no world pass has neither keywords nor formats to warm for yet.
    std::vector<WorldView> views;
    for (const Rendering::ViewDesc& view : renderServices.Views().GetViews())
    {
        const std::optional<Rendering::MaterialKeyword> keywords =
            renderServices.Views().GetViewDeclaredWorldPassKeywords(view.id);
        const Rendering::PipelineFormatKey formats = renderServices.Views().GetViewWorldColorFormatKey(view.id);
        if (keywords && formats.ColorCount != 0)
            views.push_back({*keywords, renderServices.Views().GetViewWorldReliefDepthSource(view.id), formats});
    }
    if (views.empty())
        return 0;

    size_t requested = 0;
    std::erase_if(m_Pending, [&](const NotedDraw& draw) {
        const bool done = WarmDraw(draw, renderServices, views);
        if (done)
            m_Noted.erase({static_cast<uint64_t>(draw.Mesh), draw.Material});
        requested += done && renderServices.GetMeshGPURegistry().Find(draw.Mesh) ? 1 : 0;
        return done;
    });
    return requested;
}

bool SceneDrawWarm::WarmDraw(const NotedDraw& draw, RenderServices& renderServices, const std::vector<WorldView>& views)
{
    const Rendering::MeshGPURegistry& meshes = renderServices.GetMeshGPURegistry();
    const Rendering::MeshGPUEntry* entry = meshes.Find(draw.Mesh);
    if (!entry)
        return true; // unregistered since it was noted: nothing to warm
    const Material* material = renderServices.Materials().Registry().Find(draw.Material);
    if (!material || !material->GetGraphicsPipelineId().IsValid())
        return false;
    Rendering::MeshGPUEntryBindings bindings{};
    if (!meshes.TryGetDrawableBindings(*entry, bindings))
        return false;
    const Rendering::VertexAttributeFlags bound = BoundStreamVertexFlags(*entry, bindings, material->IgnoresVertexColor());
    PipelineVariantCache& variants = renderServices.Materials().Variants();
    for (const WorldView& view : views)
        for (const SceneColorDrawKey& key :
             EnumerateSceneColorDrawKeys(*material, bound, entry->lodCount, view.Keywords, view.Relief))
            variants.WarmColorVariant(*material, key.VertexFlags, entry->topology, key.PassKeywords,
                                      renderServices.GetFrontFace(), view.Formats);
    return true;
}

} // namespace GameEngine::Engine::Renderer
