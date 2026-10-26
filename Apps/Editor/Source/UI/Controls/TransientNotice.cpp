#include "UI/Controls/TransientNotice.h"

#include "Editor/EditorPaths.h"
#include "Logger/Logger.h"
#include "UI/Controls/Label.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIStyle.h"

#include <filesystem>
#include <memory>
#include <utility>

namespace GameEngine::EditorUI
{
namespace
{

constexpr const char* kLayoutAssetPath = "UI/controls/TransientNotice/TransientNotice.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/TransientNotice/TransientNotice.css";
constexpr const char* kTextId = "TransientNoticeText";
constexpr const char* kShownClass = "transient-notice--shown";

} // namespace

TransientNotice::TransientNotice()
{
    AddClass("transient-notice");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    // A notice is built once per host panel, so the layout is read when it is built.
    const std::filesystem::path path = Editor::GetEditorGlobalPaths().installAssetsRoot / kLayoutAssetPath;
    std::unique_ptr<UIElement> plate;
    if (!UIParsing::XMLParser::ParseLayoutFromFile(path.string(), plate) || !plate)
    {
        Logger::Log::Error("TransientNotice: {} did not load, so no confirmation shows. Check that it is staged "
                           "under the editor asset mount.",
                           path.generic_string());
        return;
    }
    m_Text = dynamic_cast<Label*>(plate->FindById(HashStringId(kTextId)));
    if (!m_Text)
        Logger::Log::Error("TransientNotice: {} needs a Label with id '{}' in its plate.", kLayoutAssetPath, kTextId);
    AddChild(std::move(plate));
}

void TransientNotice::Show(const std::string& text, std::chrono::steady_clock::time_point now,
                           std::optional<float> top)
{
    if (m_Text)
        m_Text->SetText(text);
    // A placed notice overrides the sheet's bottom edge; an unplaced one returns to it.
    if (top)
        Overrides().Set(Style::PositionTop, StyleLength::Px(*top)).Set(Style::PositionBottom, StyleLength::Auto());
    else
        Overrides().Reset(Style::PositionTop).Reset(Style::PositionBottom);
    MarkDirty(LayoutDirty);
    m_HideAt = now + kShownDuration;
    if (m_Shown)
        return;
    m_Shown = true;
    AddClass(kShownClass);
}

void TransientNotice::Tick(std::chrono::steady_clock::time_point now)
{
    if (!m_Shown || now < m_HideAt)
        return;
    m_Shown = false;
    RemoveClass(kShownClass);
}

} // namespace GameEngine::EditorUI

namespace RegisterWidgets
{
static auto s_RegisterTransientNotice =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::EditorUI::TransientNotice>(
        "TransientNotice", []() { return std::make_unique<GameEngine::EditorUI::TransientNotice>(); });
}
