#include "DayCycle/DayCycleProfileControls.h"

#include "AssetCore/SharedFileRead.h"
#include "Assets/DayCycleProfile.h"
#include "DayCycle/DayCycleShippedProfiles.h"
#include "DayCycle/DayCycleUi.h"
#include "DayCycle/DayCycleWorkingCopy.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kChoiceClass = "day-cycle-axis-choice";
constexpr const char* kActiveClass = "day-cycle-axis-choice-active";

Button* AddButton(UIElement& parent, const std::string& text, const char* cssClass)
{
    Button* button = AddDayCycleChild<Button>(parent, cssClass);
    button->SetText(text);
    return button;
}

const char* SaveLabel(DayCycleSaveAction action)
{
    return action == DayCycleSaveAction::SaveInPlace ? "Save" : "Save a copy to the project";
}

// The unsaved mark beside Save, and Save itself enabled only while there is something to save (a
// copy can always be saved).
void ShowUnsavedMark(Label& caption, Button& save, const DayCycleWorkingCopy& profile)
{
    const bool dirty = profile.IsDirty();
    caption.SetText(dirty ? "Unsaved changes" : "");
    save.SetEnabled(dirty || profile.GetSaveAction() == DayCycleSaveAction::SaveCopyToProject);
}

// Keeps the caption and Save following the profile until they are gone, then stops listening.
void FollowUnsavedMark(Label& caption, Button& save, const std::shared_ptr<DayCycleWorkingCopy>& profile)
{
    auto token = std::make_shared<std::uint64_t>(0);
    std::weak_ptr<DayCycleWorkingCopy> weakProfile = profile;
    *token = profile->Subscribe([captionRef = UIElement::MakeWeakRef(&caption), saveRef = UIElement::MakeWeakRef(&save),
                                 weakProfile, token]() {
        const std::shared_ptr<DayCycleWorkingCopy> live = weakProfile.lock();
        Label* label = captionRef.Get();
        Button* button = saveRef.Get();
        if (!live)
            return;
        if (!label || !button)
        {
            live->Unsubscribe(*token);
            return;
        }
        ShowUnsavedMark(*label, *button, *live);
    });
}

void SaveOrCopy(const std::shared_ptr<DayCycleWorkingCopy>& profile, Label* caption,
                const std::function<void(const std::filesystem::path&)>& onCopied)
{
    std::string error;
    if (profile->GetSaveAction() == DayCycleSaveAction::SaveInPlace)
    {
        if (!profile->Save(error) && caption)
            caption->SetText(error);
        return;
    }
    const std::shared_ptr<DayCycleWorkingCopy> copy = profile->SaveCopy(error);
    if (!copy)
    {
        if (caption)
            caption->SetText(error);
        return;
    }
    if (caption)
        caption->SetText("Saved as " + copy->GetPath().filename().string());
    if (onCopied)
        onCopied(copy->GetPath());
}

void LoadPreset(const std::shared_ptr<DayCycleWorkingCopy>& profile, const std::filesystem::path& presetPath,
                const std::string& presetName, UndoRedoService* undo)
{
    std::string text;
    DayCycleProfileData preset;
    std::string error;
    if (!ReadFileTextShared(presetPath, text) || !ParseDayCycleProfile(text, preset, error))
        return;
    DayCycleProfileData data = profile->GetData();
    data.Tracks = preset.Tracks;
    profile->Commit(data, "Load the " + presetName + " preset", undo);
}
} // namespace

void AddDayCycleAxisToggle(UIElement& parent, DayCycleAxisView view, std::function<void(DayCycleAxisView)> onChoose)
{
    UIElement* group = AddDayCycleChild<UIElement>(parent, "day-cycle-axis-toggle");
    const std::pair<DayCycleAxisView, const char*> choices[] = {{DayCycleAxisView::Elevation, "Sun's elevation"},
                                                                {DayCycleAxisView::TimeOfDay, "Time of day"}};
    for (const auto& [choice, text] : choices)
    {
        Button* button = AddButton(*group, text, kChoiceClass);
        if (choice == view)
            button->AddClass(kActiveClass);
        button->SetOnClick([onChoose, choice = choice](UIEvent&) {
            if (onChoose)
                onChoose(choice);
        });
    }
}

UIElement& AddDayCycleSaveButton(UIElement& parent, const std::shared_ptr<DayCycleWorkingCopy>& profile,
                           std::function<void(const std::filesystem::path&)> onCopied)
{
    UIElement* group = AddDayCycleChild<UIElement>(parent, "day-cycle-save");
    Label* caption = AddDayCycleChild<Label>(*group, "day-cycle-save-caption");
    Button* save = AddButton(*group, SaveLabel(profile->GetSaveAction()), "day-cycle-save-button");
    ShowUnsavedMark(*caption, *save, *profile);
    FollowUnsavedMark(*caption, *save, profile);
    save->SetOnClick([profile, ref = UIElement::MakeWeakRef(caption), onCopied = std::move(onCopied)](UIEvent&) {
        SaveOrCopy(profile, ref.Get(), onCopied);
    });
    return *group;
}

void AddDayCyclePresets(UIElement& parent, AssetManager& assets, const std::shared_ptr<DayCycleWorkingCopy>& profile,
                        UndoRedoService* undo)
{
    UIElement* group = AddDayCycleChild<UIElement>(parent, "day-cycle-presets");
    Label* title = AddDayCycleChild<Label>(*group, "day-cycle-presets-title");
    title->SetText("Presets (loads the preset's tracks)");
    for (const std::filesystem::path& path : ShippedDayCycleProfiles(assets))
    {
        std::string text;
        DayCycleProfileData preset;
        std::string error;
        if (!ReadFileTextShared(path, text) || !ParseDayCycleProfile(text, preset, error))
            continue;
        Button* button = AddButton(*group, preset.Name, "day-cycle-preset");
        button->SetOnClick([profile, path, name = preset.Name, undo](UIEvent&) { LoadPreset(profile, path, name, undo); });
    }
}

} // namespace GameEngine::Editor
