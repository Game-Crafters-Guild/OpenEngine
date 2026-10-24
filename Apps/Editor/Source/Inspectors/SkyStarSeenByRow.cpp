#include "Inspectors/SkyStarSeenByRow.h"

#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/World.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Sky/StarLookPresets.h"
#include "UI/Controls/PictureChoiceField.h"

#include <memory>
#include <span>
#include <vector>

namespace GameEngine
{
namespace
{

constexpr const char* kSeenByTooltip =
    "Who looks at the sky. Camera, the default: no glare, and every star keeps its color. Eye: an eye used to the "
    "dark, with a glare around bright stars, and faint stars that fade to white. The presets do not change it.";

struct SeenByChoice
{
    Components::StarObserverKind Observer;
    const char* Label;
    const char* Description;
    const char* PictureClass;
};

// The tiles in order: a tile's index is its index here.
constexpr SeenByChoice kSeenByChoices[] = {
    {Components::StarObserverKind::Camera, "Camera", "Every star keeps its color.",
     "option-picture--star-seen-by-camera"},
    {Components::StarObserverKind::Eye, "Eye", "Faint stars look white.", "option-picture--star-seen-by-eye"},
};

size_t SeenByIndex(Components::StarObserverKind observer)
{
    const std::span<const SeenByChoice> choices = kSeenByChoices;
    for (size_t i = 0; i < choices.size(); ++i)
    {
        if (choices[i].Observer == observer)
            return i;
    }
    return 0;
}

void CommitSeenBy(size_t choice, ECS::World* world, ECS::EntityHandle sky,
                  Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo)
{
    const std::span<const SeenByChoice> choices = kSeenByChoices;
    if (!world || choice >= choices.size())
        return;
    ApplyStarObserver(choices[choice].Observer, *world, sky, notifications, undo);
}

} // namespace

void AddSkyStarSeenByRow(UIElement* parent, ECS::World* world, ECS::EntityHandle sky,
                         Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo)
{
    const auto* skyValues = world ? world->GetComponent<Components::SkyEnvironment>(sky) : nullptr;
    if (!skyValues)
        return;
    std::vector<EditorUI::PictureChoiceField::Choice> choices;
    for (const SeenByChoice& choice : kSeenByChoices)
        choices.push_back({choice.Label, choice.Description, choice.PictureClass});
    auto field = std::make_unique<EditorUI::PictureChoiceField>(std::move(choices), SeenByIndex(skyValues->StarObserver));
    field->SetOnSelected([world, sky, notifications, undo](size_t choice)
                         { CommitSeenBy(choice, world, sky, notifications, undo); });
    InspectorUI::AddFieldRow(parent, "Seen by", std::move(field), kSeenByTooltip);
}

} // namespace GameEngine
