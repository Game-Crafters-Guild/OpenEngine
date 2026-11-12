#include "Inspectors/BuiltInInspectorRegistration.h"

#include "Components/Navigation/NavigationSource.h"
#include "Components/Rendering/DDGIVolume.h"
#include "Components/SpatialGridMover.h"
#include "ECS/ECS.h"
#include "Editor/Entities/EditorComponentTraits.h"

#include <array>
#include <string_view>
#include <utility>

namespace GameEngine
{

namespace
{

// The editor traits of a component that needs only names and style classes. An empty text keeps the default.
struct DataOnlyComponentTraits
{
    ECS::ComponentTypeId TypeId = 0;
    std::string_view DisplayName;
    std::string_view InspectorCategory;
    std::string_view InspectorTooltip;
    std::string_view InspectorIconClass;
    std::string_view HierarchyRowClass;
};

constexpr std::array kDataOnlyComponentTraits = {
    DataOnlyComponentTraits{
        .TypeId = ECS::GetComponentTypeId<Components::SpatialGridMover>(),
        .DisplayName = "Spatial Grid Mover",
        .InspectorCategory = "AI",
        .InspectorTooltip = "The spatial grid indexes this entity as a sphere at its position, so AI senses, "
                            "triggers and combat find it with a query.",
    },
    DataOnlyComponentTraits{
        .TypeId = ECS::GetComponentTypeId<Components::NavigationSource>(),
        .DisplayName = "Navigation Source",
        .InspectorCategory = "Navigation",
        .InspectorTooltip = "Whether the navigation bake reads the meshes of this entity and of its children that "
                            "set no policy of their own. Colliders keep their own rule.",
    },
    // Both classes resolve to the same icon in views.css, so the inspector section and the hierarchy row agree.
    DataOnlyComponentTraits{
        .TypeId = ECS::GetComponentTypeId<Components::DDGIVolume>(),
        .DisplayName = "DDGI Volume",
        .InspectorIconClass = "inspector-section-icon-ddgi-volume",
        .HierarchyRowClass = "hierarchy-entity-ddgi-volume",
    },
};

} // namespace

void RegisterBuiltInComponentTraits()
{
    for (const DataOnlyComponentTraits& entry : kDataOnlyComponentTraits)
    {
        Editor::EditorComponentTraits traits;
        traits.DisplayName = entry.DisplayName;
        traits.InspectorCategory = entry.InspectorCategory;
        traits.InspectorTooltip = entry.InspectorTooltip;
        traits.InspectorIconClass = entry.InspectorIconClass;
        traits.HierarchyRowClass = entry.HierarchyRowClass;
        Editor::EditorComponentTraitsRegistry::Get().Register(entry.TypeId, std::move(traits));
    }
}

} // namespace GameEngine
