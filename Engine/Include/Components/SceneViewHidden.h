#pragma once

namespace GameEngine::Components
{

// The Scene View eye, set by the Hierarchy's eye column (Editor::SetHiddenInSceneView): the
// entity is hidden in the editor's Scene View and nowhere else. Extraction gives its mesh instances
// Rendering::kInstanceFlagEditorHidden, and only the Scene View's culls drop that flag, its own
// shadow culls included: the Game View and the Player still draw the entity and its shadow.
// Physics, scripts and the enable state are unaffected. Editor view state (design decision D12):
// not undoable, never written to a scene or a prefab, so a scene reload shows everything again;
// play mode keeps it.
//
// @ge-no-add        set by the Hierarchy's eye, never added by hand
// [DoNotSerialize]  editor view state, not scene data
struct SceneViewHidden
{
    // An editor view switch, not a feature: it has no off state of its own.
    static constexpr bool NotToggleable = true;

    // Presence is the signal; the field exists because the reflection scanner reflects (and so
    // marks DoNotSerialize) only a component that has one.
    bool Active = true;
};

// Derived: the entity or an ancestor carries SceneViewHidden. Never set by hand;
// SceneViewHiddenInHierarchySystem derives it from the Parent chain, so a child follows its parent's
// eye and keeps its own state for when the parent shows again.
//
// @ge-no-add        derived by SceneViewHiddenInHierarchySystem
// [DoNotSerialize]  derived editor view state
struct SceneViewHiddenInHierarchy
{
    static constexpr bool NotToggleable = true;

    // See SceneViewHidden::Active.
    bool Active = true;
};

} // namespace GameEngine::Components
