#pragma once

#include <string>

namespace GameEngine
{
class Dropdown;

/// The AI Assistant panel's connection dropdown: it offers every connection, shows the stored
/// one and stores the one the user picks (AiAssistantSettings::SetProvider). The open list is as
/// wide as its longest name, within the panel. The tooltip's first line is the whole name of the
/// selected connection, which a narrow panel cuts in the closed dropdown.
class AgentConnectionDropdown
{
public:
    /// Fills `dropdown` with the connections; the dropdown must outlive this object.
    explicit AgentConnectionDropdown(Dropdown& dropdown);

    /// Selects the connection `providerId` and names it in the tooltip.
    void Show(const std::string& providerId);

private:
    Dropdown& m_Dropdown;
};

} // namespace GameEngine
