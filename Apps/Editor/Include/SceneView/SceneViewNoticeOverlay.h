#pragma once

#include "UI/UIElement.h"
#include "UI/ViewOverlay.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
class Label;
struct EditorContext;
namespace EditorUI
{
class InspectorNotice;
} // namespace EditorUI
} // namespace GameEngine

namespace GameEngine::Editor
{

/// Shows `text` along the top of the Scene View for a few seconds, replacing a notice shown now:
/// why an action the user just took did nothing, and what to do instead. Main thread only.
void ShowSceneViewNotice(std::string text);

/// Shows `text` with a button, `actionLabel`, that calls `onAction` and hides the notice. The notice
/// stays longer than one without a button, so the user can reach it. Main thread only.
void ShowSceneViewNoticeWithAction(std::string text, std::string actionLabel, std::string actionTooltip,
                                   std::function<void()> onAction);

/// Draws the notice ShowSceneViewNotice raised, a banner in the Scene View's overlay layer, or the
/// shared notice control with its button for ShowSceneViewNoticeWithAction. Hidden once its time is up,
/// and when the scene changes.
class SceneViewNoticeOverlay final : public ViewOverlay
{
  public:
    explicit SceneViewNoticeOverlay(const EditorContext& context);

    void Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera) override;
    void Update() override;

  private:
    // The two elements of one Scene View viewport: the banner for a notice without a button, and the
    // shared notice control for a notice with one.
    struct View
    {
        UIElement::WeakRef<Label> Banner;
        UIElement::WeakRef<EditorUI::InspectorNotice> ActionNotice;
    };

    const EditorContext& m_Context;
    std::vector<View> m_Views;
    // The world's handle run at the last update. A scene change starts a new one.
    std::optional<uint64_t> m_WorldHandleEpoch;
    uint64_t m_ShownSerial = 0;
    bool m_Visible = false;
};

} // namespace GameEngine::Editor
