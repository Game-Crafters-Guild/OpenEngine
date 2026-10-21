#include "Editor/Registries/MarkupActionRegistry.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Editor
{

MarkupActionRegistry& MarkupActionRegistry::Get()
{
    static MarkupActionRegistry s_Instance;
    return s_Instance;
}

void MarkupActionRegistry::Register(MarkupAction action)
{
    if (action.Id.empty() || action.Label.empty() || !action.Action)
    {
        Logger::Log::Error("MarkupActionRegistry: rejecting action '{}': Id, Label and Action are required",
                           action.Id);
        return;
    }
    if (action.Label.find('/') != std::string::npos)
    {
        Logger::Log::Error("MarkupActionRegistry: rejecting action '{}': its label '{}' holds a '/', which the "
                           "Mark-ups panel's context menu would read as a submenu; name it without one",
                           action.Id, action.Label);
        return;
    }
    const auto existing = std::find_if(m_Actions.begin(), m_Actions.end(),
                                       [&](const Entry& entry) { return entry.Action.Id == action.Id; });
    if (existing != m_Actions.end())
    {
        existing->Action = std::move(action);
        existing->Module = ECS::GetActiveRegistrationModule();
        return;
    }
    m_Actions.push_back(Entry{std::move(action), ECS::GetActiveRegistrationModule()});
}

std::vector<const MarkupAction*> MarkupActionRegistry::GetActions() const
{
    std::vector<const MarkupAction*> actions;
    actions.reserve(m_Actions.size());
    for (const Entry& entry : m_Actions)
        actions.push_back(&entry.Action);
    return actions;
}

void MarkupActionRegistry::AppendModulePins(std::string_view moduleId, std::vector<std::string>& outPins) const
{
    if (moduleId.empty())
        return;
    for (const Entry& entry : m_Actions)
    {
        if (entry.Module.ModuleId == moduleId)
            outPins.push_back("mark-up action '" + entry.Action.Id + "'");
    }
}

} // namespace GameEngine::Editor
