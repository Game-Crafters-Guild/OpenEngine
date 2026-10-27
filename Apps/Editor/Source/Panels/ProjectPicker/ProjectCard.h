#pragma once

#include "UI/Controls/Button.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace GameEngine
{
namespace Editor
{
struct ProjectCatalogEntry;
}

/// One card of the project picker: a library card (`project-card`: New project, Open project, a recent project) or a
/// catalog card (`project-template-card`). The layouts and ProjectCard.css are in UI/controls/ProjectPicker/.
///
/// The Button is the hit target and does not move: its `project-card-lift` child holds the look and moves on hover.
class ProjectCard final : public Button
{
  public:
    enum class Action : uint8_t
    {
        NewProject,
        OpenProject,
    };

    /// The library card that starts `action`. The layout holds its text.
    explicit ProjectCard(Action action);
    /// The library card of the recent project at `index` in the grid: `thumbnailPath` on the preview tint of
    /// `index`, then `name` and `meta`. An empty `thumbnailPath` shows the scene placeholder.
    ProjectCard(size_t index, const std::string& name, const std::string& meta, const std::string& thumbnailPath);
    /// The catalog card of `entry` with the id `elementId`: the entry's thumbnail on its accent color, or on the
    /// preview tint of `tintIndex` when the entry has no accent color, then the entry's name and `meta`.
    ProjectCard(const std::string& elementId, const Editor::ProjectCatalogEntry& entry, const std::string& meta,
                size_t tintIndex);

  private:
    /// Adds `rootClass`, the shared classes and the sheet.
    void AddCardLook(const char* rootClass);
    /// Adds the class of the preview tint of `index`. ProjectCard.css holds the tints.
    void AddPreviewTint(size_t index);
    /// Loads the thumbnail layout and sets its text and thumbnail. Returns the preview, or null when the layout
    /// does not load or its name or meta is not a Label.
    UIElement* AddThumbnailContent(const std::string& name, const std::string& meta, const std::string& thumbnailPath,
                                   BackgroundSizeMode thumbnailSize);
};

} // namespace GameEngine
