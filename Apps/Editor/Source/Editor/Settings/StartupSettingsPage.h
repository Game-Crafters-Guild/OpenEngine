#pragma once

namespace GameEngine::Editor
{
class SettingsStore;

/// Whether the user turned on "Auto-load last project" in the editor preferences.
/// It is off by default, so the editor shows the project picker on start.
bool IsAutoLoadLastProjectOn(const SettingsStore& preferences);

/// Registers the Startup settings category (what the editor opens on start).
void RegisterStartupSettingsCategory();

} // namespace GameEngine::Editor
