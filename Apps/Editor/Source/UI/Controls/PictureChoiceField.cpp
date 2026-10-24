#include "UI/Controls/PictureChoiceField.h"

#include "Input/KeyCodes.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Logger/Logger.h"
#include "UI/Controls/Label.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/UIEvents.h"

#include <memory>
#include <utility>

namespace GameEngine::EditorUI
{
namespace
{

constexpr const char* kLayoutAssetPath = "UI/controls/PictureChoiceField/PictureChoiceField.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/PictureChoiceField/PictureChoiceField.css";
constexpr const char* kPictureClass = "picture-choice-picture";
constexpr const char* kTextClass = "picture-choice-text";
constexpr const char* kNameClass = "picture-choice-name";
constexpr const char* kDescriptionClass = "picture-choice-description";
constexpr const char* kSelectedClass = "picture-choice-tile-selected";

} // namespace

PictureChoiceField::PictureChoiceField(std::vector<Choice> choices, size_t selectedIndex)
    : m_SelectedIndex(selectedIndex < choices.size() ? selectedIndex : 0)
{
    AddClass("picture-choice-field");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    // One Tab stop for the group, as for radio buttons; the arrows move the selection.
    SetFocusable(true);
    // Read once per process: an inspector rebuild builds the field again, and parsing the few nodes of a
    // tile is cheap where a disk read each time is not.
    static const std::string s_LayoutText = InspectorUI::ReadEditorLayoutText(kLayoutAssetPath);
    for (size_t i = 0; i < choices.size(); ++i)
        AddTile(s_LayoutText, choices[i], i);
    SyncSelectedClasses();
}

void PictureChoiceField::AddTile(const std::string& layoutText, const Choice& choice, size_t index)
{
    std::unique_ptr<UIElement> tile;
    if (layoutText.empty() || !UIParsing::XMLParser::ParseLayoutFromString(layoutText, tile, kLayoutAssetPath) || !tile)
        return;

    UIElement* picture = InspectorDrag::FindChildByClass(tile.get(), kPictureClass);
    UIElement* text = InspectorDrag::FindChildByClass(tile.get(), kTextClass);
    auto* name = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(text, kNameClass));
    auto* description = dynamic_cast<Label*>(InspectorDrag::FindChildByClass(text, kDescriptionClass));
    if (!picture || !name || !description)
    {
        Logger::Log::Error("PictureChoiceField: {} needs a '{}' element and the Labels '{}' and '{}' in '{}'.",
                           kLayoutAssetPath, kPictureClass, kNameClass, kDescriptionClass, kTextClass);
        return;
    }
    if (!choice.PictureClass.empty())
        picture->AddClass(choice.PictureClass);
    name->SetText(choice.Label);
    description->SetText(choice.Description);
    for (UIElement* part : {tile.get(), picture, text, static_cast<UIElement*>(name), static_cast<UIElement*>(description)})
        part->SetFocusProxy(this);

    tile->RegisterEventHandler(kEventMouseDown, [this, index](UIEvent& e) { OnTilePressed(e, index); });
    m_Tiles.push_back(tile.get());
    AddChild(std::move(tile));
}

void PictureChoiceField::OnTilePressed(UIEvent& e, size_t index)
{
    // The left button only, as for a radio button: a right-click selects nothing.
    if (e.Button != Input::kMouseButton_Left || !IsEnabled())
        return;
    Select(index);
    e.Stop();
}

void PictureChoiceField::SetOnSelected(std::function<void(size_t)> onSelected)
{
    m_OnSelected = std::move(onSelected);
}

void PictureChoiceField::OnEvent(UIEvent& e)
{
    if (e.Id == kEventKeyDown && IsEnabled() && !m_Tiles.empty())
    {
        // Up and Left select the previous tile, Down and Right the next, wrapping at the ends, as the
        // arrows do in a group of radio buttons.
        const size_t count = m_Tiles.size();
        const bool previous = e.Key == Input::kKeyCode_Left || e.Key == Input::kKeyCode_Up;
        const bool next = e.Key == Input::kKeyCode_Right || e.Key == Input::kKeyCode_Down;
        if (previous || next)
        {
            Select(previous ? (m_SelectedIndex + count - 1) % count : (m_SelectedIndex + 1) % count);
            e.Stop();
            return;
        }
    }
    UIElement::OnEvent(e);
}

void PictureChoiceField::Select(size_t index)
{
    if (index >= m_Tiles.size() || index == m_SelectedIndex)
        return;
    m_SelectedIndex = index;
    SyncSelectedClasses();
    if (m_OnSelected)
        m_OnSelected(index);
}

void PictureChoiceField::SyncSelectedClasses()
{
    for (size_t i = 0; i < m_Tiles.size(); ++i)
    {
        if (i == m_SelectedIndex)
            m_Tiles[i]->AddClass(kSelectedClass);
        else
            m_Tiles[i]->RemoveClass(kSelectedClass);
    }
}

} // namespace GameEngine::EditorUI
