#include "Editor/Entities/EntityDisplayName.h"

#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Types/StringUtils.h"

namespace GameEngine::Editor
{

std::string EntityDisplayName(const ECS::World& world, ECS::EntityHandle entity)
{
    if (const auto* name = world.GetComponent<Components::Name>(entity); name && !name->View().empty())
        return std::string(name->View());
    if (const ECS::Archetype* archetype = world.GetEntityArchetype(entity))
    {
        const EditorComponentTraitsRegistry& traits = EditorComponentTraitsRegistry::Get();
        std::string untitled = traits.UntitledNameOf(archetype->GetSignature().GetComponents());
        if (!untitled.empty())
            return untitled;
    }
    return EntityIdLabel(entity);
}

std::string EntityIdLabel(ECS::EntityHandle entity)
{
    return "Entity " + std::to_string(entity.id);
}

std::string ComponentTypeNameAsWords(std::string_view registeredTypeName)
{
    std::string_view name = registeredTypeName;
    if (const size_t scope = name.rfind("::"); scope != std::string_view::npos)
        name.remove_prefix(scope + 2);
    if (const size_t dot = name.rfind('.'); dot != std::string_view::npos)
        name.remove_prefix(dot + 1);
    return IdentifierToWords(name);
}

} // namespace GameEngine::Editor
