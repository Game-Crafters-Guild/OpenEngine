#pragma once

namespace GameEngine
{
struct InspectorContext;
}

namespace GameEngine::Editor
{
struct DayCycleEditorServices;

/// The Day Cycle profile asset's inspector: the name, the description, the axis toggle, Save (or Save
/// a copy to the project for a shipped profile), the track rows on the working copy the Day Cycle
/// window shares, and the presets. A track the profile leaves out reads "not keyed by this profile".
void BuildDayCycleProfileInspector(const InspectorContext& context, const DayCycleEditorServices& services);

/// Registers the inspector for AssetType::DayCycleProfile over the editor's own services.
void RegisterDayCycleProfileInspector();

} // namespace GameEngine::Editor
