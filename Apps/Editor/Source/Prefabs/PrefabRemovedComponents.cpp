#include "Prefabs/PrefabRemovedComponents.h"

#include "Inspectors/ComponentIconClass.h"
#include "UI/InspectorSection.h"

#include "Scene/SceneSchemaRegistry.h"
#include "Types/StringUtils.h"

#include <memory>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kRemovedTooltip = "The prefab has this component. This instance removed it.";

} // namespace

PrefabRemovedComponents::PrefabRemovedComponents(const std::vector<std::string>& components)
{
    AddClass("prefab-removed-components");
    for (const std::string& component : components)
    {
        auto section = std::make_unique<InspectorSection>(IdentifierToWords(component));
        // The icon the component's live header shows.
        const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find(component);
        const ECS::ComponentTypeId typeId = schema ? Scene::SceneSchemaRegistry::ComponentTypeForSchema(*schema) : 0;
        section->SetHeaderIconClass(ComponentIconClass(nullptr, {}, typeId));
        section->SetEnabledTogglePlaceholder(true);
        section->SetOverridden(true);
        section->Fade();
        section->HideOptions();
        section->SetHeaderTooltip(kRemovedTooltip);
        section->SetHeaderBadge("Removed", kRemovedTooltip, InspectorSection::BadgeKind::Neutral);
        AddChild(std::move(section));
    }
}

} // namespace GameEngine::Editor
