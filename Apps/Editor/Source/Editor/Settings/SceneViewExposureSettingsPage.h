#pragma once

namespace GameEngine::Editor
{

// Registers the Scene View's exposure rows (mode, Fixed EV100, Auto envelope, compensation) with
// Editor::EditorSettingsRegistry, inline on the Scene View page. Main-thread only, once at editor
// startup; the SettingsPanel builds the rows from the descriptor.
void RegisterSceneViewExposureSettingsCategory();

} // namespace GameEngine::Editor
