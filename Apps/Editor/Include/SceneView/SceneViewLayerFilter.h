#pragma once

#include "Rendering/RenderLayerMask.h"

#include <filesystem>

namespace GameEngine::Editor
{

/// The render layers the Scene View draws: every layer by default, set from the Scene View toolbar's layer
/// field. Per user and per project, in the user's project settings (key sceneView.visibleLayers), because
/// the layer names it refers to are the project's; never in a scene. Every Scene View pane and its camera
/// preview draw these layers. The Game View follows its camera and ignores the filter. Main thread.
class SceneViewLayerFilter
{
  public:
    static constexpr const char* kKey = "sceneView.visibleLayers";

    static SceneViewLayerFilter& Get();

    /// The layers for the project at `workspaceRoot`, read from its user settings the first time that
    /// project asks.
    Rendering::RenderLayerMask Layers(const std::filesystem::path& workspaceRoot);

    /// Makes `layers` the filter for the project at `workspaceRoot` and saves it.
    void SetLayers(const std::filesystem::path& workspaceRoot, Rendering::RenderLayerMask layers);

  private:
    SceneViewLayerFilter() = default;

    // The saved filter of the project at `workspaceRoot`, or every layer when none is saved.
    static Rendering::RenderLayerMask Load(const std::filesystem::path& workspaceRoot);
    // Saves `layers` as the filter of the project at `workspaceRoot`. False when nothing could be written.
    static bool Save(const std::filesystem::path& workspaceRoot, Rendering::RenderLayerMask layers);

    std::filesystem::path m_Root;
    bool m_Loaded = false;
    Rendering::RenderLayerMask m_Layers = Rendering::RenderLayerMask::All();
};

} // namespace GameEngine::Editor
