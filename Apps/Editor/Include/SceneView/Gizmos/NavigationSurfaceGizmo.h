#pragma once

namespace GameEngine::Editor::SceneTools
{

/// Registers the Scene View gizmo of a NavigationSurface with ComponentGizmoRegistry: a selected or
/// hovered surface draws what it baked, mesh or grid alike, as a faint tint over the walkable
/// surface with its edge outlined, and, when its bounds are explicit, their box. Called by the
/// surface's inspector registration.
void RegisterNavigationSurfaceGizmo();

} // namespace GameEngine::Editor::SceneTools
