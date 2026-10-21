// Reflection for NavigationSource: the Add Component entry, the inspector's reflected row, the scene
// file and set_component all read this table. The component lives under the module's own include
// tree, which the build-time component scanner does not read, so it is registered here.

#include "PathfindingECS/Components/NavigationSource.h"

#include "Components/ComponentRegistration.h"

GE_REFLECT_ENUM_TABLE_BEGIN(NavigationSourcePolicy)
    GE_REFLECT_ENUM_TABLE_VALUE("Auto", 0)
    GE_REFLECT_ENUM_TABLE_VALUE("Include", 1)
    GE_REFLECT_ENUM_TABLE_VALUE("Exclude", 2)
GE_REFLECT_ENUM_TABLE_END()

GE_REGISTER_COMPONENT_BEGIN(GameEngine::Components::NavigationSource)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::NavigationSource, Policy)
GE_REGISTER_COMPONENT_END(GameEngine::Components::NavigationSource)

GE_REFLECT_ENUM_FIELD(GameEngine::Components::NavigationSource, Policy, NavigationSourcePolicy)

GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::NavigationSource, Policy,
                         "Whether the navigation bake reads this entity's mesh and the meshes of its children "
                         "that set no policy of their own. Auto: when neither it nor a parent moves. Include: always, "
                         "where it stands when the bake runs; a child that moves on its own needs its own "
                         "Include. Exclude: never; colliders still bake.")
