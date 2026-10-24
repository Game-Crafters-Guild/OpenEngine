#include "Inspectors/SkyStarLookRow.h"

#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Sky/StarLookPresets.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{

constexpr const char* kPresetTooltip =
    "Sets the star settings below and the night tint on every global Post Process Volume, in one undo step. "
    "Reads Custom when a star setting or the night tint differs from every preset. Does not change Seen by.";
constexpr const char* kCustomText = "Custom";
constexpr const char* kNoVolumeCaption =
    "The night tint needs a global Post Process Volume: without one, a preset sets the star settings only. Add one "
    "to set the tint.";
constexpr const char* kTintCaption =
    "The night tint on the Post Process Volume differs from every preset. Pick a preset to reset it.";

std::vector<Dropdown::Option> PresetOptions()
{
    std::vector<Dropdown::Option> options;
    for (const StarLookPreset& preset : StarLookPresets())
        options.push_back({preset.Name, preset.Name, {}, {}, preset.PictureClass, preset.Description});
    return options;
}

// The index of the look the sky's star dials and the night tint match in the Preset list, or -1 (Custom).
int MatchingLookIndex(const Components::SkyEnvironment* sky, ECS::World& world)
{
    const StarLookPreset* look = sky ? FindStarLook(*sky, world) : nullptr;
    return look ? static_cast<int>(look - StarLookPresets().data()) : -1;
}

// The caption under the list, or null for none: the missing volume, or the night tint when it alone
// makes the field read Custom (the star dials are a look's), so the user sees what to change.
const char* CaptionText(const Components::SkyEnvironment* sky, ECS::World* world)
{
    if (!world)
        return nullptr;
    if (!HasGlobalPostProcessVolume(*world))
        return kNoVolumeCaption;
    if (!sky || FindStarLook(*sky, *world))
        return nullptr;
    const std::span<const StarLookPreset> presets = StarLookPresets();
    const bool dialsAreALook = std::any_of(presets.begin(), presets.end(), [sky](const StarLookPreset& preset)
                                           { return MatchesStarLook(*sky, preset); });
    return dialsAreALook ? kTintCaption : nullptr;
}

// The Preset list and its caption, kept in step with the sky's dials and the volumes' night tint.
class StarLookField final : public UIElement
{
  public:
    StarLookField(ECS::World* world, ECS::EntityHandle sky, Editor::EditorChangeNotifications* notifications,
                  Editor::UndoRedoService* undo)
        : m_World(world)
        , m_Sky(sky)
        , m_Notifications(notifications)
        , m_Undo(undo)
    {
        AddClass("inspector-field");
        auto presets = std::make_unique<Dropdown>();
        m_Presets = presets.get();
        m_Presets->AddClass("inspector-dropdown");
        m_Presets->SetNoSelectionText(kCustomText);
        m_Presets->SetOptions(PresetOptions());
        AddChild(std::move(presets));
        auto caption = std::make_unique<Label>();
        caption->AddClass("inspector-field-caption");
        m_Caption = caption.get();
        AddChild(std::move(caption));
        Refresh();
        m_Presets->SetOnValueChanged([this](const std::string&) { OnPresetPicked(); });
        if (m_Notifications)
        {
            m_Subscription = m_Notifications->SubscribeComponentChanged(
                [this](const Editor::EditorChangeNotifications::ComponentChangedEvent& event) { OnChanged(event); });
            m_WorldSubscription = m_Notifications->SubscribeWorldStructureChanged(
                [this](const Editor::EditorChangeNotifications::WorldStructureChangedEvent& event) { OnWorldChanged(event); });
        }
    }

    ~StarLookField() override
    {
        if (m_Notifications && m_Subscription)
            m_Notifications->Unsubscribe(m_Subscription);
        if (m_Notifications && m_WorldSubscription)
            m_Notifications->Unsubscribe(m_WorldSubscription);
    }

  private:
    void OnPresetPicked()
    {
        const int index = m_Presets->GetSelectedIndex();
        const std::span<const StarLookPreset> presets = StarLookPresets();
        if (!m_World || index < 0 || static_cast<size_t>(index) >= presets.size())
            return;
        ApplyStarLookPreset(presets[static_cast<size_t>(index)], *m_World, m_Sky, m_Notifications, m_Undo);
        // The dial rows show the values they were built with: rebuild the inspector so they show the look's.
        if (m_Notifications)
            m_Notifications->NotifyComponentChange<Components::SkyEnvironment>(
                m_World, m_Sky, Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
    }

    void OnChanged(const Editor::EditorChangeNotifications::ComponentChangedEvent& event)
    {
        if (event.world != m_World)
            return;
        const bool skyChanged = event.entity == m_Sky &&
                                event.componentType == ECS::GetComponentTypeId<Components::SkyEnvironment>();
        const bool volumeChanged = event.componentType == ECS::GetComponentTypeId<Components::PostProcessVolume>();
        if (skyChanged || volumeChanged)
            Refresh();
    }

    void OnWorldChanged(const Editor::EditorChangeNotifications::WorldStructureChangedEvent& event)
    {
        if (event.world == m_World)
            Refresh();
    }

    void Refresh()
    {
        const auto* sky = m_World ? m_World->GetComponent<Components::SkyEnvironment>(m_Sky) : nullptr;
        // An open list belongs to the user: the selection is not moved under the pointer.
        if (!m_Presets->IsMenuOpen())
            m_Presets->SetSelectedIndexWithoutNotify(m_World ? MatchingLookIndex(sky, *m_World) : -1);
        const char* caption = CaptionText(sky, m_World);
        if (!caption)
        {
            m_Caption->AddClass("hidden");
            return;
        }
        m_Caption->SetText(caption);
        m_Caption->RemoveClass("hidden");
    }

    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Sky{};
    Editor::EditorChangeNotifications* m_Notifications = nullptr;
    Editor::UndoRedoService* m_Undo = nullptr;
    Editor::EditorChangeNotifications::SubscriptionToken m_Subscription{};
    Editor::EditorChangeNotifications::SubscriptionToken m_WorldSubscription{};
    Dropdown* m_Presets = nullptr;
    Label* m_Caption = nullptr;
};

} // namespace

void AddSkyStarLookRow(UIElement* parent, ECS::World* world, ECS::EntityHandle sky,
                       Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo)
{
    UIElement* row = InspectorUI::AddRow(parent);
    if (!row)
        return;
    InspectorUI::AddLabel(row, "Preset", kPresetTooltip);
    row->AddChild(std::make_unique<StarLookField>(world, sky, notifications, undo));
}

} // namespace GameEngine
