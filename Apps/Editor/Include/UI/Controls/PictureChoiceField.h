#pragma once

#include "UI/UIElement.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine::EditorUI
{

/// A choice among a few options that look different, shown side by side as tiles: a picture of what
/// the option does, its name and one line under it. One tile is selected at a time, like a group of
/// radio buttons: a left click on a tile selects it, and with the field focused (one Tab stop) Up and
/// Left select the previous tile, Down and Right the next, wrapping at the ends.
///
/// A tile is declared in UI/controls/PictureChoiceField/PictureChoiceField.uxml and styled by
/// PictureChoiceField.css beside it. The picture is a CSS class, as for a dropdown option: the
/// stylesheet (theme/option-pictures.css) says which image it is. The tiles are built in the
/// constructor, so they are in the tree from the frame the field is built. The field shows the index
/// it was built with: the inspector builds it again on an undo or a redo.
class PictureChoiceField : public UIElement
{
public:
    struct Choice
    {
        std::string Label;
        /// One line under the name: what the choice does.
        std::string Description;
        /// The CSS class of the choice's picture.
        std::string PictureClass;
    };

    PictureChoiceField(std::vector<Choice> choices, size_t selectedIndex);

    /// Called with the index the user selected; not called when the user selects the selected tile.
    void SetOnSelected(std::function<void(size_t)> onSelected);

    void OnEvent(UIEvent& e) override;

private:
    void AddTile(const Choice& choice, size_t index);
    void OnTilePressed(UIEvent& e, size_t index);
    void Select(size_t index);
    void SyncSelectedClasses();

    std::vector<UIElement*> m_Tiles;
    size_t m_SelectedIndex = 0;
    std::function<void(size_t)> m_OnSelected;
};

} // namespace GameEngine::EditorUI
