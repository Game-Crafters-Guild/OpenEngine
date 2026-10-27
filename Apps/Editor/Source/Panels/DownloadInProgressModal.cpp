#include "Panels/DownloadInProgressModal.h"

#include "Assets/PolyhavenDownloadManager.h"
#include "Input/KeyCodes.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <array>
#include <memory>
#include <string_view>

namespace GameEngine
{

namespace
{
constexpr const char* kLayoutAssetPath = "UI/controls/DownloadInProgressModal/DownloadInProgressModal.uxml";
// The ids the constructor looks up. A layout that lacks one does not load.
constexpr std::array<std::string_view, 5> kRequiredIds = {
    "download-progress-title",
    "download-progress-message",
    "download-progress-cancel",
    "download-progress-proceed",
    "download-progress-wait",
};
} // namespace

DownloadInProgressModal::DownloadInProgressModal()
{
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);

    std::unique_ptr<UIElement> backdrop = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!backdrop)
        return;
    m_Backdrop = backdrop.get();
    m_Backdrop->SetFocusable(true);
    m_Title = m_Backdrop->FindById("download-progress-title"_sid);
    m_Message = m_Backdrop->FindById("download-progress-message"_sid);
    if (auto* cancel = dynamic_cast<Button*>(m_Backdrop->FindById("download-progress-cancel"_sid)))
        cancel->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });
    if (auto* proceed = dynamic_cast<Button*>(m_Backdrop->FindById("download-progress-proceed"_sid)))
        proceed->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnProceedClicked(); });
    if (auto* wait = dynamic_cast<Button*>(m_Backdrop->FindById("download-progress-wait"_sid)))
        wait->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnWaitClicked(); });

    m_Backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (m_Visible && e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            OnCancelClicked();
        }
    });

    AddChild(std::move(backdrop));
}

void DownloadInProgressModal::Show(const std::string& /*actionName*/,
                                    PolyhavenDownloadManager* downloadManager,
                                    std::function<void()> onProceed)
{
    if (!m_Backdrop)
        return;
    m_Visible = true;
    m_DownloadManager = downloadManager;
    m_OnProceed = std::move(onProceed);

    if (auto* t = dynamic_cast<Label*>(m_Title))
        t->SetText("Downloads In Progress");

    UpdateMessage();

    AddClass("visible");
    if (auto* manager = GetOwnerManager())
    {
        UIElement* backdrop = m_Backdrop;
        manager->PostToUI([manager, backdrop]() { manager->FocusElement(backdrop); });
    }
}

void DownloadInProgressModal::Hide()
{
    m_Visible = false;
    m_DownloadManager = nullptr;
    m_OnProceed = nullptr;
    RemoveClass("visible");
}

void DownloadInProgressModal::Poll()
{
    if (!m_Visible || !m_DownloadManager)
        return;

    UpdateMessage();

    // Auto-dismiss when all downloads complete.
    if (!m_DownloadManager->HasActiveDownloads())
    {
        auto proceed = std::move(m_OnProceed);
        Hide();
        if (proceed)
            proceed();
    }
}

void DownloadInProgressModal::OnWaitClicked()
{
    // Keep modal visible — Poll() will auto-dismiss when done.
}

void DownloadInProgressModal::OnProceedClicked()
{
    auto proceed = std::move(m_OnProceed);
    Hide();
    if (proceed)
        proceed();
}

void DownloadInProgressModal::OnCancelClicked()
{
    Hide();
}

void DownloadInProgressModal::UpdateMessage()
{
    if (!m_DownloadManager || !m_Message)
        return;

    uint32_t count = m_DownloadManager->ActiveDownloadCount();
    std::vector<std::string> slugs;
    m_DownloadManager->GetActiveDownloadSlugs(slugs);

    std::string text = std::to_string(count) + " download" + (count != 1 ? "s" : "") + " in progress";
    if (!slugs.empty())
    {
        text += ": ";
        for (size_t i = 0; i < slugs.size() && i < 5; ++i)
        {
            if (i > 0) text += ", ";
            text += slugs[i];
        }
        if (slugs.size() > 5)
            text += "...";
    }
    text += "\n\nWait for downloads to finish, or proceed anyway with placeholders in the scene.";

    if (auto* lbl = dynamic_cast<Label*>(m_Message))
        lbl->SetText(text);
}

} // namespace GameEngine
