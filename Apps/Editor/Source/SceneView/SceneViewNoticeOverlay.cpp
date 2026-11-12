#include "SceneView/SceneViewNoticeOverlay.h"

#include "ECS/Entity.h"
#include "EditorContext.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/StyleUtil.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr std::chrono::seconds kNoticeDuration{4};
// A notice with a button stays longer, so the user can read it and reach the button.
constexpr std::chrono::seconds kActionNoticeDuration{10};

// The notice raised last. Serial counts the notices raised, so the overlay restarts the banner
// for a notice with the same text as the one before it. OnAction is empty for a notice with no button.
struct RaisedNotice
{
    std::string Text;
    std::string ActionLabel;
    std::string ActionTooltip;
    std::function<void()> OnAction;
    std::chrono::steady_clock::time_point Until{};
    uint64_t Serial = 0;
};

RaisedNotice& Raised()
{
    static RaisedNotice s_Raised;
    return s_Raised;
}

// The button's click: the notice goes away, then the action runs.
void InvokeRaisedAction()
{
    RaisedNotice& raised = Raised();
    const std::function<void()> action = std::move(raised.OnAction);
    raised.OnAction = {};
    raised.Until = std::chrono::steady_clock::now();
    if (action)
        action();
}

// Hides the notice raised last and drops its button's action.
void DropRaisedNotice()
{
    RaisedNotice& raised = Raised();
    raised.OnAction = {};
    raised.Until = std::chrono::steady_clock::now();
}
} // namespace

void ShowSceneViewNotice(std::string text)
{
    RaisedNotice& raised = Raised();
    raised.Text = std::move(text);
    raised.ActionLabel.clear();
    raised.ActionTooltip.clear();
    raised.OnAction = {};
    raised.Until = std::chrono::steady_clock::now() + kNoticeDuration;
    ++raised.Serial;
}

void ShowSceneViewNoticeWithAction(std::string text, std::string actionLabel, std::string actionTooltip,
                                   std::function<void()> onAction)
{
    RaisedNotice& raised = Raised();
    raised.Text = std::move(text);
    raised.ActionLabel = std::move(actionLabel);
    raised.ActionTooltip = std::move(actionTooltip);
    raised.OnAction = std::move(onAction);
    raised.Until = std::chrono::steady_clock::now() + kActionNoticeDuration;
    ++raised.Serial;
}

SceneViewNoticeOverlay::SceneViewNoticeOverlay(const EditorContext& context)
    : m_Context(context)
{
}

void SceneViewNoticeOverlay::Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& /*camera*/)
{
    if (view != ViewOverlayView::Scene)
        return;
    auto banner = std::make_unique<Label>();
    banner->AddClass("view-overlay-banner");
    banner->AddClass("scene-view-notice");
    UIUtil::SetShown(*banner, false);
    auto actionNotice =
        std::make_unique<EditorUI::InspectorNotice>(std::string(), EditorUI::InspectorNotice::Kind::Information);
    actionNotice->AddClass("inspector-notice-opaque");
    actionNotice->AddClass("scene-view-action-notice");
    UIUtil::SetShown(*actionNotice, false);
    m_Views.push_back({UIElement::MakeWeakRef(banner.get()), UIElement::MakeWeakRef(actionNotice.get())});
    layer.AddChild(std::move(banner));
    layer.AddChild(std::move(actionNotice));
    m_ShownSerial = 0;
    m_Visible = false;
}

void SceneViewNoticeOverlay::Update()
{
    std::erase_if(m_Views, [](const View& shown) { return !shown.Banner.Get() || !shown.ActionNotice.Get(); });
    // A scene change clears the world and starts a new handle run. A notice drawn before it, and its
    // button, belong to the scene that is gone. A notice raised after the change stays.
    if (m_Context.World)
    {
        const uint64_t epoch = m_Context.World->GetEntityHandleEpoch();
        if (m_WorldHandleEpoch && *m_WorldHandleEpoch != epoch && Raised().Serial == m_ShownSerial)
            DropRaisedNotice();
        m_WorldHandleEpoch = epoch;
    }
    const RaisedNotice& raised = Raised();
    const bool visible = raised.Serial != 0 && std::chrono::steady_clock::now() < raised.Until;
    if (visible == m_Visible && raised.Serial == m_ShownSerial)
        return;
    m_Visible = visible;
    m_ShownSerial = raised.Serial;
    const bool withAction = static_cast<bool>(raised.OnAction);
    for (const View& shown : m_Views)
    {
        Label* banner = shown.Banner.Get();
        EditorUI::InspectorNotice* actionNotice = shown.ActionNotice.Get();
        banner->SetText(raised.Text);
        actionNotice->SetText(raised.Text);
        if (withAction)
            actionNotice->SetAction(raised.ActionLabel, raised.ActionTooltip, InvokeRaisedAction);
        // One element of the two draws the notice; the other stays hidden.
        UIElement* drawn = withAction ? static_cast<UIElement*>(actionNotice) : banner;
        UIElement* unused = withAction ? static_cast<UIElement*>(banner) : actionNotice;
        UIUtil::SetShown(*unused, false);
        UIUtil::SetShown(*drawn, visible);
    }
}

} // namespace GameEngine::Editor
