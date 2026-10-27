#include "Panels/ProjectPicker/ProjectCard.h"

#include "Logger/Logger.h"
#include "Projects/ProjectCatalog.h"
#include "UI/ControlLayoutLoader.h"
#include "UI/Controls/Label.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"

#include <array>
#include <memory>
#include <string_view>

namespace GameEngine
{

namespace
{
constexpr const char* kThumbnailLayoutAssetPath = "UI/controls/ProjectPicker/ProjectCard.uxml";
constexpr const char* kNewProjectLayoutAssetPath = "UI/controls/ProjectPicker/RecentProjectNewCard.uxml";
constexpr const char* kOpenProjectLayoutAssetPath = "UI/controls/ProjectPicker/RecentProjectOpenCard.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/ProjectPicker/ProjectCard.css";
constexpr const char* kProjectPlaceholderPath = "Icons/project-scene-placeholder.svg";
constexpr const char* kLibraryCardClass = "project-card";
constexpr const char* kCatalogCardClass = "project-template-card";
// ProjectCard.css holds one tint class per index: project-card-tint-0 to project-card-tint-4.
constexpr size_t kPreviewTintCount = 5;
// The ids the thumbnail constructors look up in their layout. A layout that lacks one does not load.
constexpr std::array<std::string_view, 4> kThumbnailRequiredIds = {
    "project-card-preview",
    "project-card-preview-image",
    "project-card-name",
    "project-card-meta",
};
} // namespace

ProjectCard::ProjectCard(Action action)
{
    const bool newProject = action == Action::NewProject;
    SetId(newProject ? "recent-project-card-new" : "recent-project-card-open");
    AddCardLook(kLibraryCardClass);
    AddClass("project-card-action");
    AddClass(newProject ? "project-card-new" : "project-card-open");

    std::unique_ptr<UIElement> lift = Editor::ControlLayoutLoader::Load(
        newProject ? kNewProjectLayoutAssetPath : kOpenProjectLayoutAssetPath, {});
    if (!lift)
        return;
    AddChild(std::move(lift));
}

ProjectCard::ProjectCard(size_t index, const std::string& name, const std::string& meta,
                         const std::string& thumbnailPath)
{
    SetId("recent-project-card-" + std::to_string(index));
    AddCardLook(kLibraryCardClass);
    AddPreviewTint(index);

    UIElement* preview = AddThumbnailContent(name, meta, thumbnailPath, BackgroundSizeMode::Cover);
    if (!preview)
        return;
    preview->SetId("recent-preview-" + std::to_string(index));
}

ProjectCard::ProjectCard(const std::string& elementId, const Editor::ProjectCatalogEntry& entry,
                         const std::string& meta, size_t tintIndex)
{
    SetId(elementId);
    AddCardLook(kCatalogCardClass);
    AddPreviewTint(tintIndex);

    UIElement* preview = AddThumbnailContent(
        entry.Name, meta, entry.ThumbnailRef,
        entry.ThumbnailContain ? BackgroundSizeMode::Contain : BackgroundSizeMode::Cover);
    if (preview && entry.AccentColor != 0)
        preview->Overrides().Set(Style::BackgroundColor, entry.AccentColor);
}

void ProjectCard::AddCardLook(const char* rootClass)
{
    AddClass(rootClass);
    AddClass("project-card-animated");
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
}

void ProjectCard::AddPreviewTint(size_t index)
{
    AddClass("project-card-tint-" + std::to_string(index % kPreviewTintCount));
}

UIElement* ProjectCard::AddThumbnailContent(const std::string& name, const std::string& meta,
                                            const std::string& thumbnailPath, BackgroundSizeMode thumbnailSize)
{
    std::unique_ptr<UIElement> lift =
        Editor::ControlLayoutLoader::Load(kThumbnailLayoutAssetPath, kThumbnailRequiredIds);
    if (!lift)
        return nullptr;
    Label* nameLabel = dynamic_cast<Label*>(lift->FindById("project-card-name"_sid));
    Label* metaLabel = dynamic_cast<Label*>(lift->FindById("project-card-meta"_sid));
    if (!nameLabel || !metaLabel)
    {
        Logger::Log::Error("{}: the element with the id '{}' is not a Label, so the project card is empty.",
                           kThumbnailLayoutAssetPath, nameLabel ? "project-card-meta" : "project-card-name");
        return nullptr;
    }
    UIElement* previewImage = lift->FindById("project-card-preview-image"_sid);
    UI::Layout::SetBackgroundPath(*previewImage, thumbnailPath.empty() ? kProjectPlaceholderPath : thumbnailPath);
    previewImage->Overrides().Set(Style::BackgroundSize, BackgroundSizeValue{thumbnailSize});

    nameLabel->SetText(name);
    metaLabel->SetText(meta);
    UIElement* preview = lift->FindById("project-card-preview"_sid);
    AddChild(std::move(lift));
    return preview;
}

} // namespace GameEngine
