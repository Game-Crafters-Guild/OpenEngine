#pragma once

namespace GameEngine::Components
{

// Marks the ground plane that a project template or a new scene starts with. A new terrain switches
// the entity off in the same undo step (Editor::CreateTerrainEntity); the entity stays.
// @ge-tag
// @ge-no-add       set by the templates and the new-scene seed, never added by hand
// @ge-editor-only  the editor's authoring aid, no game data
struct TemplateGroundPlaceholder
{
    // A marker, not a feature: it has no off state.
    static constexpr bool NotToggleable = true;
};

} // namespace GameEngine::Components
