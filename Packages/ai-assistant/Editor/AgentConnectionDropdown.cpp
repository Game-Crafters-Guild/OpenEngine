#include "AgentConnectionDropdown.h"

#include "AiAssistantSettings.h"

#include "UI/Controls/Dropdown.h"

#include <string>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr const char* kTooltip = "The assistant that answers your prompts. Changing it starts a new conversation. "
                                 "Choose the model with the button to its right. Set up connections and API keys "
                                 "in Settings > AI Assistant.";
} // namespace

AgentConnectionDropdown::AgentConnectionDropdown(Dropdown& dropdown) : m_Dropdown(dropdown)
{
    std::vector<Dropdown::Option> connections;
    for (const AiAssistantSettings::ProviderChoice& choice : AiAssistantSettings::ProviderChoices())
        connections.push_back({std::string(choice.Id), std::string(choice.Label), ""});
    m_Dropdown.SetOptions(connections, 0);
    m_Dropdown.SetAutoWidthPopup(true);
    m_Dropdown.SetTooltipPlacement(UIElement::TooltipPlacement::Right);
    m_Dropdown.SetOnValueChanged([](const std::string& providerId) { AiAssistantSettings::SetProvider(providerId); });
}

void AgentConnectionDropdown::Show(const std::string& providerId)
{
    m_Dropdown.SetSelectedValue(providerId);
    m_Dropdown.SetTooltip(std::string(AiAssistantSettings::ProviderChoiceOf(providerId).Label) + "\n" + kTooltip);
}

} // namespace GameEngine
