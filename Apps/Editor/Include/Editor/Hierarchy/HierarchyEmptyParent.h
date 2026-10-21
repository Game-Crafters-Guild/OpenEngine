#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "Components/Name.h"
#include "ECS/Entity.h"

#include <memory>
#include <span>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

class EditorChangeNotifications;

struct EmptyParentResult
{
    // Invalid when nothing was created.
    ECS::EntityHandle NewParent{};
    // The one undo step for the creation and the moves, already applied.
    std::unique_ptr<IEditorCommand> Command;
};

// Creates one empty entity named `name` and moves `entities` under it, the
// Hierarchy's "Create Empty Parent".
//
// - An entity whose ancestor is also given stays where it is; it moves with that ancestor.
// - The new parent goes under the entities' parent when they share one, otherwise
//   under their lowest common ancestor (the scene root when there is none), in the
//   sibling place of the first entity in tree order, or of the branch holding it.
// - It sits at the centre of the entities' world positions with identity rotation
//   and unit scale in world space. Every moved entity keeps its world pose, and the
//   moved entities keep their tree order.
//
// Creates nothing when no live entity is given, or when the new parent's place is
// a generated entity, which takes no authored children (CanAcceptAuthoredChildren).
EmptyParentResult CreateEmptyParent(ECS::World& world,
                                    EditorChangeNotifications* notifications,
                                    std::span<const ECS::EntityHandle> entities,
                                    const Components::Name& name);

} // namespace GameEngine::Editor
