#include "Editor/Registries/EditorServiceRegistry.h"

#include "Editor/Registries/EditorService.h"
#include "Placement/RecipeEditorHooks.h"

#include <cassert>

namespace GameEngine::Editor
{

EditorServiceRegistry::EditorServiceRegistry()
{
    Register(&RecipeEditorHooks::Create);
}

EditorServiceRegistry::~EditorServiceRegistry()
{
    Stop();
}

void EditorServiceRegistry::Register(Factory factory)
{
    assert(m_Services.empty() && "register the editor services before the schedule starts");
    m_Factories.push_back(std::move(factory));
}

void EditorServiceRegistry::Start(const EditorServiceContext& context)
{
    assert(m_Services.empty() && "the editor services started twice");
    for (const Factory& factory : m_Factories)
    {
        if (std::unique_ptr<IEditorService> service = factory(context))
            m_Services.push_back(std::move(service));
    }
}

void EditorServiceRegistry::ReleaseSceneScoped(ECS::World& world)
{
    for (const std::unique_ptr<IEditorService>& service : m_Services)
        service->ReleaseSceneScoped(world);
}

void EditorServiceRegistry::Stop()
{
    while (!m_Services.empty())
        m_Services.pop_back();
}

} // namespace GameEngine::Editor
