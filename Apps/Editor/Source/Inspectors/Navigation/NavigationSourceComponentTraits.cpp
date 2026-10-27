#include "Inspectors/Navigation/NavigationSourceComponentTraits.h"

#include "ECS/ECS.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Components/Navigation/NavigationSource.h"

#include <utility>

namespace GameEngine
{

void RegisterNavigationSourceComponentTraits()
{
    Editor::EditorComponentTraits traits;
    traits.DisplayName = "Navigation Source";
    traits.InspectorCategory = "Navigation";
    traits.InspectorTooltip = "Whether the navigation bake reads the meshes of this entity and of its children that "
                              "set no policy of their own. Colliders keep their own rule.";
    Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::NavigationSource>(),
                                                          std::move(traits));
}

} // namespace GameEngine
