#pragma once

#include "ECS/ECS.h"

#include <functional>
#include <vector>

namespace GameEngine
{
class UIElement;
struct UIEvent;
} // namespace GameEngine

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Editor
{

/// What a Hierarchy row's eye asks of the panel that holds the row.
struct HierarchyRowEyeHost
{
    /// The world the rows show, or null.
    std::function<ECS::World*()> World;
    /// The entity `row` shows now, or an invalid handle: the tree recycles rows.
    std::function<ECS::EntityHandle(const UIElement& row)> RowEntity;
    /// The entities one click on `clicked`'s eye sets: the selection when the row is in it.
    std::function<std::vector<ECS::EntityHandle>(ECS::EntityHandle clicked)> ClickTargets;
    /// Shows `row`'s new state after a click.
    std::function<void(UIElement& row, ECS::EntityHandle entity)> RowChanged;
};

/// The Scene View eye on a Hierarchy row: the row's `hierarchy-eye` child and its click
/// (SetHiddenInSceneView). HierarchyRowActivity shows the eye's state on it.
class HierarchyRowEye
{
  public:
    explicit HierarchyRowEye(HierarchyRowEyeHost host);

    /// Gives `row` its eye; once per pooled row.
    void AddTo(UIElement& row);

    /// Shows the eye on `row`, bound to `entity`, only when the eye has something to hide there: the
    /// entity has a mesh, or it has children that may. A leaf with no mesh shows none.
    void Present(UIElement& row, ECS::EntityHandle entity, bool hasChildren);

  private:
    void OnMouseDown(UIElement& row, UIEvent& e);

    HierarchyRowEyeHost m_Host;
};

} // namespace GameEngine::Editor
