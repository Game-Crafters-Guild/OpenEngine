#pragma once

#include "UI/UIElement.h"

namespace GameEngine
{

class Button;
class Label;

namespace Editor
{
class PlayModeEntryGate;
}

// "Entering Play Mode": shown while the play-mode entry gate holds an entry for the script build,
// with what is building, how far it is, and Cancel; after a failed build it says why play mode
// did not start and offers Close. Layout: UI/controls/PlayModeEntryModal/PlayModeEntryModal.uxml.
class PlayModeEntryModal final : public UIElement
{
  public:
    explicit PlayModeEntryModal(Editor::PlayModeEntryGate& gate);

    // Per frame: shows, updates or hides the dialog to match the gate (PlayModeManager advances it).
    void Poll();

  private:
    void Present();
    void SetVisible(bool visible);
    void OnButtonClicked();

    Editor::PlayModeEntryGate& m_Gate;
    UIElement* m_Backdrop = nullptr;
    Label* m_Title = nullptr;
    Label* m_Message = nullptr;
    Label* m_Activity = nullptr;
    UIElement* m_ProgressTrack = nullptr;
    UIElement* m_ProgressFill = nullptr;
    Button* m_Button = nullptr;
    bool m_Visible = false;
};

} // namespace GameEngine
