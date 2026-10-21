#include "PromptMicButton.h"

#include "UI/UIEvents.h"

#include <utility>

namespace GameEngine
{

void PromptMicButton::SetHold(std::function<bool()> enabled, std::function<void()> begin,
                              std::function<void()> end)
{
    m_HoldEnabled = std::move(enabled);
    m_BeginHold = std::move(begin);
    m_EndHold = std::move(end);
}

void PromptMicButton::OnEvent(UIEvent& event)
{
    const bool holdMode = m_HoldEnabled && m_HoldEnabled();
    if (holdMode && event.Id == kEventMouseDown && event.Button == 0)
    {
        m_Holding = true;
        event.Capture(this);
        AddClass("pressed");
        if (m_BeginHold)
            m_BeginHold();
        event.Stop();
        return;
    }
    if (m_Holding && ((event.Id == kEventMouseUp && event.Button == 0) || event.Id == kEventMouseCancel))
    {
        m_Holding = false;
        RemoveClass("pressed");
        if (m_EndHold)
            m_EndHold();
        event.Stop();
        return;
    }
    if (m_Holding && event.Id == kEventMouseMove)
    {
        event.Stop();
        return;
    }
    Button::OnEvent(event);
}

} // namespace GameEngine
