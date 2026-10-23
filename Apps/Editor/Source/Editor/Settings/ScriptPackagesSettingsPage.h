#pragma once

namespace GameEngine::Editor
{

// Registers the "Script Packages" project-settings page with Editor::EditorSettingsRegistry: the NuGet
// packages of the project's scripts, read from and written to the PackageReference items of
// GameEngine.Scripts.csproj, the same file the IDE edits. Main-thread only, once at editor startup.
void RegisterScriptPackagesSettingsCategory();

} // namespace GameEngine::Editor
