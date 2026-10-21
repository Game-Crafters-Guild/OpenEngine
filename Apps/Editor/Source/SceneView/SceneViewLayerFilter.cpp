#include "SceneView/SceneViewLayerFilter.h"

#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"
#include "Scene/RenderLayerMaskText.h"

#include <string>

namespace GameEngine::Editor
{

SceneViewLayerFilter& SceneViewLayerFilter::Get()
{
    static SceneViewLayerFilter s_Instance;
    return s_Instance;
}

Rendering::RenderLayerMask SceneViewLayerFilter::Layers(const std::filesystem::path& workspaceRoot)
{
    if (!m_Loaded || m_Root != workspaceRoot)
    {
        m_Root = workspaceRoot;
        m_Layers = Load(workspaceRoot);
        m_Loaded = true;
    }
    return m_Layers;
}

void SceneViewLayerFilter::SetLayers(const std::filesystem::path& workspaceRoot, Rendering::RenderLayerMask layers)
{
    m_Root = workspaceRoot;
    m_Layers = layers;
    m_Loaded = true;
    if (!Save(workspaceRoot, layers))
        Logger::Log::Warning("SceneViewLayerFilter: the Scene View layers hold for this session only: the project "
                             "has no writable user settings file");
}

Rendering::RenderLayerMask SceneViewLayerFilter::Load(const std::filesystem::path& workspaceRoot)
{
    if (workspaceRoot.empty())
        return Rendering::RenderLayerMask::All();
    SettingsStore store = OpenUserProjectSettings(workspaceRoot);
    std::string error;
    (void)store.Load(&error);
    std::string text;
    Rendering::RenderLayerMask layers = Rendering::RenderLayerMask::All();
    if (store.TryGetString(kKey, text) && !Scene::ParseRenderLayerMask(text, kKey, layers, &error))
        Logger::Log::Warning("SceneViewLayerFilter: {}; the Scene View draws every layer", error);
    return layers;
}

bool SceneViewLayerFilter::Save(const std::filesystem::path& workspaceRoot, Rendering::RenderLayerMask layers)
{
    if (workspaceRoot.empty())
        return false;
    SettingsStore store = OpenUserProjectSettings(workspaceRoot);
    std::string error;
    (void)store.Load(&error);
    store.SetString(kKey, Scene::FormatRenderLayerMask(layers));
    return store.Save(&error);
}

} // namespace GameEngine::Editor
