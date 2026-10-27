#include "Panels/AddTagModal.h"

#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/EditorTags.h"
#include "UI/StyleProperties.h"
#include "UI/Registration/ElementRegistration.h"
#include "Input/KeyCodes.h"

#include <array>
#include <memory>
#include <string_view>

namespace GameEngine {

namespace {

constexpr const char* kLayoutAssetPath = "UI/controls/AddTagModal/AddTagModal.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/AddTagModal/AddTagModal.css";
constexpr const char* kListLayoutAssetPath = "UI/controls/AddTagModal/AddTagModalList.uxml";
constexpr const char* kTagRowLayoutAssetPath = "UI/controls/AddTagModal/AddTagModalTagRow.uxml";
constexpr const char* kTagCircleClass = "add-tag-modal-circle";
constexpr const char* kTagLabelClass = "add-tag-modal-label";
// The circle of a tag that has a color. The circle of a tag without a color has the base style.
constexpr const char* kTagColoredCircleClass = "colored";
// The ids the constructor looks up in each layout. A layout that lacks one does not load.
constexpr std::array<std::string_view, 3> kRequiredIds = {
    "add-tag-window",
    "add-tag-header",
    "add-tag-scroll",
};
constexpr std::array<std::string_view, 4> kRequiredListIds = {
    "add-tag-all-row",
    "add-tag-new-section",
    "add-tag-new-name",
    "add-tag-new-add",
};

}

AddTagModal::AddTagModal()
{
    AddClass("add-tag-modal");
    SetOverlayLayer(OverlayLayer::BlockingDialog);
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");

    std::unique_ptr<UIElement> backdrop = Editor::ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    std::unique_ptr<UIElement> list = Editor::ControlLayoutLoader::Load(kListLayoutAssetPath, kRequiredListIds);
    if (!backdrop || !list)
        return;
    auto* scroll = dynamic_cast<ScrollView*>(backdrop->FindById("add-tag-scroll"_sid));
    if (!scroll)
        return;
    m_Backdrop = backdrop.get();
    backdrop->SetFocusable(true);
    backdrop->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        if (e.Button == 0 && e.Target == m_Backdrop && !IsWindowDragging())
            OnBackdropClicked();
    });
    backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
        if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            Hide();
        }
    });
    m_Window = backdrop->FindById("add-tag-window"_sid);
    BindDragHandle(backdrop->FindById("add-tag-header"_sid));

    m_TagListContainer = list.get();
    m_AllTagsRow = list->FindById("add-tag-all-row"_sid);
    m_AllTagsRow->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
        if (e.Button == 0)
            OnAllTagsClicked();
    });
    m_AddNewSection = list->FindById("add-tag-new-section"_sid);
    m_NewTagNameField = dynamic_cast<TextField*>(list->FindById("add-tag-new-name"_sid));
    m_AddTagButton = dynamic_cast<Button*>(list->FindById("add-tag-new-add"_sid));
    if (m_AddTagButton)
        m_AddTagButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnAddNewTagClicked(); });
    // A scroll view takes its content through its viewport, which a layout file cannot name.
    scroll->AddContent(std::move(list));

    AddChild(std::move(backdrop));
    PlaceWindow(0.0f, 0.0f);
}

void AddTagModal::GetWindowOrigin(float& x, float& y) const
{
    x = m_WindowOffsetX;
    y = m_WindowOffsetY;
}

void AddTagModal::PlaceWindow(float x, float y)
{
    m_WindowOffsetX = x;
    m_WindowOffsetY = y;
    if (!m_Window)
        return;
    // Flex centering from backdrop handles the 50% base; margins apply the offset.
    m_Window->Overrides()
        .Set(Style::MarginLeft, StyleLength::Px(m_WindowOffsetX))
        .Set(Style::MarginTop, StyleLength::Px(m_WindowOffsetY));
}

void AddTagModal::Show(const std::vector<std::filesystem::path>& paths)
{
    m_Paths = paths;
    CancelWindowDrag();
    PlaceWindow(0.0f, 0.0f);
    if (m_NewTagNameField)
        m_NewTagNameField->SetValue("");
    AddClass("visible");
    if (auto* manager = GetOwnerManager())
    {
        UIElement* backdrop = m_Backdrop;
        manager->PostToUI([manager, backdrop]() { manager->FocusElement(backdrop); });
    }
    // Defer building the tag list so the modal appears immediately and the UI stays responsive.
    PostAction([this]() { BuildTagList(); });
}

void AddTagModal::Hide()
{
    RemoveClass("visible");
}

void AddTagModal::BuildTagList()
{
    if (!m_TagListContainer)
        return;

    // Take out "All Tags…" and "Add new" section so we can re-add after tag rows
    std::unique_ptr<UIElement> allTagsRowPtr = m_TagListContainer->TakeChild(m_AllTagsRow);
    std::unique_ptr<UIElement> addNewPtr = m_TagListContainer->TakeChild(m_AddNewSection);

    // Remove any existing tag rows from a previous BuildTagList() so we don't duplicate.
    // This whole function must run outside event dispatch, which is why both
    // callers post it: TakeChild returns null during dispatch, so the two rows
    // held above would be dropped and this loop would never end.
    while (!m_TagListContainer->GetChildren().empty())
        (void)m_TagListContainer->TakeChild(m_TagListContainer->GetChildren()[0].get());

    const std::vector<EditorTagDefinition> tags = EditorTags::Load();
    for (const auto& tag : tags)
    {
        std::unique_ptr<UIElement> row = Editor::ControlLayoutLoader::Load(kTagRowLayoutAssetPath, {});
        if (!row)
            break;
        UIElement* circle = InspectorDrag::FindChildByClass(row.get(), kTagCircleClass);
        auto* label = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(row.get(), kTagLabelClass));
        if (!circle || !label)
        {
            Logger::Log::Error("Tags: {} needs the '{}' element and the '{}' label.", kTagRowLayoutAssetPath,
                               kTagCircleClass, kTagLabelClass);
            break;
        }
        const std::string tagName = tag.Name;
        row->RegisterEventHandler(kEventMouseDown, [this, tagName](UIEvent& e) {
            if (e.Button == 0)
                OnTagRowClicked(tagName);
        });

        if (!tag.Color.empty())
        {
            circle->AddClass(kTagColoredCircleClass);
            circle->Overrides().Set(Style::BackgroundColor, UIRegistration::Parser<uint32_t>::Parse(tag.Color));
        }
        label->SetText(tag.Name);

        m_TagListContainer->AddChild(std::move(row));
    }

    m_TagListContainer->AddChild(std::move(allTagsRowPtr));
    m_TagListContainer->AddChild(std::move(addNewPtr));
}

void AddTagModal::OnTagRowClicked(const std::string& tagName)
{
    if (m_OnAssignTag && !m_Paths.empty())
        m_OnAssignTag(m_Paths, tagName);
    Hide();
}

void AddTagModal::OnAllTagsClicked()
{
    Hide();
    if (m_OnOpenSettings)
        m_OnOpenSettings();
}

void AddTagModal::OnAddNewTagClicked()
{
    if (!m_NewTagNameField)
        return;
    std::string name = m_NewTagNameField->GetValue();
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
        name.pop_back();
    for (size_t i = 0; i < name.size(); )
        if (name[i] == ' ' || name[i] == '\t')
            name.erase(i, 1);
        else
            ++i;
    if (name.empty())
        return;
    m_NewTagNameField->SetValue("");
    // Defer Load/Save/BuildTagList so the click handler returns and the UI stays responsive.
    PostAction([this, name]() {
        std::vector<EditorTagDefinition> tags = EditorTags::Load();
        tags.push_back({name, "#95a5a6"});
        if (EditorTags::Save(tags))
            BuildTagList();
    });
}

void AddTagModal::OnBackdropClicked()
{
    Hide();
}

} // namespace GameEngine
