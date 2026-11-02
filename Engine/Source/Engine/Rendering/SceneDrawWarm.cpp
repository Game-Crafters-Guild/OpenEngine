#include "SceneDrawWarm.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Logger/Logger.h"

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
    if (!m_Noted.insert({static_cast<uint64_t>(mesh), materialGuid}).second)
        return;
    m_Pending.push_back({mesh, materialGuid});
    if (m_EpisodeOpen)
        return;
    m_EpisodeOpen = true;
    m_EpisodeStart = std::chrono::steady_clock::now();
    m_EpisodeFrames = 0;
    m_EpisodeDraws = 0;
}

size_t SceneDrawWarm::Step(RenderServices& renderServices)
{
    if (!m_EpisodeOpen)
        return 0;
    ++m_EpisodeFrames;
    if (m_Pending.empty())
    {
        SettleEpisode(renderServices);
        return 0;
    }
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
        if (!done)
            return false;
        m_Noted.erase({static_cast<uint64_t>(draw.Mesh), draw.Material});
        if (renderServices.GetMeshGPURegistry().Find(draw.Mesh))
        {
            ++requested;
            m_Awaiting.Insert(draw.Material);
        }
        return true;
    });
    m_EpisodeDraws += static_cast<uint32_t>(requested);
    SettleEpisode(renderServices);
    return requested;
}

void SceneDrawWarm::SettleEpisode(RenderServices& renderServices)
{
    const MaterialSystem& materials = renderServices.Materials();
    std::vector<GUID> built;
    for (const GUID& guid : m_Awaiting)
    {
        const Material* material = materials.Registry().Find(guid);
        if (!material || !materials.Variants().HasPipelineBuildsInFlight(*material))
            built.push_back(guid);
    }
    for (const GUID& guid : built)
        m_Awaiting.Erase(guid);
    if (!m_Pending.empty() || !m_Awaiting.Empty())
        return;
    m_EpisodeOpen = false;
    // An episode that settles in the frame that requested it waited for nothing (a warm draw noted
    // again, a unit spawned with a warm material): no line.
    if (m_EpisodeFrames <= 1)
        return;
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_EpisodeStart).count();
    Logger::Log::Info("Scene draw warm: the color pipelines of {} draw(s) are ready {:.0f} ms after the first "
                      "draw was noted, over {} frame(s)",
                      m_EpisodeDraws, ms, m_EpisodeFrames);
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
