#pragma once

namespace GameEngine::Editor
{

// Registers the "Render Layers" project-settings page with Editor::EditorSettingsRegistry: one name row per
// layer. Main-thread only, once at editor startup; the SettingsPanel builds the rows from the descriptor.
void RegisterRenderLayersSettingsCategory();

} // namespace GameEngine::Editor
