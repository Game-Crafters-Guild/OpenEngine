#include "Panels/CameraCustomAspectModal.h"

#include "Input/KeyCodes.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/FloatField.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>

namespace GameEngine
{

namespace
{
constexpr float kMinAspectDimension = 0.0001f;
constexpr const char* kLayoutAssetPath = "UI/controls/CameraCustomAspectModal/CameraCustomAspectModal.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/CameraCustomAspectModal/CameraCustomAspectModal.css";
// The ids the constructor looks up. A layout that lacks one does not load.
constexpr std::array<std::string_view, 4> kRequiredIds = {
    "camera-aspect-width",
    "camera-aspect-height",
    "camera-aspect-cancel",
    "camera-aspect-apply",
};
} // namespace

CameraCustomAspectModal::CameraCustomAspectModal()
{
    AddClass("camera-aspect-modal");
    SetOverlayLayer(OverlayLayer::BlockingDialog);
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    std::unique_ptr<UIElement> backdrop = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!backdrop)
        return;
    m_Backdrop = backdrop.get();
    m_WidthField = dynamic_cast<FloatField*>(m_Backdrop->FindById("camera-aspect-width"_sid));
    m_HeightField = dynamic_cast<FloatField*>(m_Backdrop->FindById("camera-aspect-height"_sid));
    m_Backdrop->FindById("camera-aspect-cancel"_sid)
        ->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });
    m_Backdrop->FindById("camera-aspect-apply"_sid)
        ->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCommitClicked(); });

    m_Backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (!m_Visible)
            return;
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            e.Handled = true;
            OnCommitClicked();
        }
        else if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            OnCancelClicked();
        }
    });

    AddChild(std::move(backdrop));
}

void CameraCustomAspectModal::Show(float initialWidth, float initialHeight)
{
    m_Visible = true;
    if (m_WidthField)
        m_WidthField->SetValue(std::max(initialWidth, kMinAspectDimension));
    if (m_HeightField)
        m_HeightField->SetValue(std::max(initialHeight, kMinAspectDimension));

    AddClass("visible");

    if (m_WidthField)
    {
        if (auto* mgr = GetOwnerManager())
        {
            FloatField* field = m_WidthField;
            mgr->PostToUI([mgr, field]()
            {
                mgr->FocusElement(field);
                field->SelectAll();
            });
        }
        else
        {
            m_WidthField->SelectAll();
        }
    }
}

void CameraCustomAspectModal::Hide()
{
    m_Visible = false;
    RemoveClass("visible");
}

void CameraCustomAspectModal::OnCommitClicked()
{
    if (!m_WidthField || !m_HeightField)
        return;

    const float width = std::max(m_WidthField->GetValue(), kMinAspectDimension);
    const float height = std::max(m_HeightField->GetValue(), kMinAspectDimension);
    Hide();
    if (m_OnCommit)
        m_OnCommit(width, height);
}

void CameraCustomAspectModal::OnCancelClicked()
{
    Hide();
    if (m_OnCancel)
        m_OnCancel();
}

} // namespace GameEngine
