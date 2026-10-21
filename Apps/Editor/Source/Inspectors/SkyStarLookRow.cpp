#include "Inspectors/SkyStarLookRow.h"

#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Sky/StarLookPresets.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"

#include <memory>
#include <span>
#include <vector>

namespace GameEngine
{
namespace
{

constexpr const char* kCustomCaption = "Custom: the star dials differ from every look.";
constexpr const char* kNoVolumeCaption =
    "The night tint needs a global Post Process Volume: without one, a look sets the star dials only. Add one to "
    "set the tint.";

void OnLookClicked(const StarLookPreset& preset, ECS::World* world, ECS::EntityHandle sky,
                   Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo)
{
    if (!world)
        return;
    ApplyStarLookPreset(preset, *world, sky, notifications, undo);
    // The dial rows show the values they were built with: rebuild the inspector so they show the look's.
    if (notifications)
        notifications->NotifyComponentChange<Components::SkyEnvironment>(
            world, sky, Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
}

void SetShown(Label* label, bool shown)
{
    if (shown)
        label->RemoveClass("hidden");
    else
        label->AddClass("hidden");
}

// The look buttons, the Custom caption and the missing-volume notice, kept in step with the sky's dials.
class StarLookField final : public UIElement
{
  public:
    StarLookField(ECS::World* world, ECS::EntityHandle sky, Editor::EditorChangeNotifications* notifications,
                  Editor::UndoRedoService* undo)
        : m_World(world)
        , m_Sky(sky)
        , m_Notifications(notifications)
    {
        AddClass("inspector-field");
        auto buttons = std::make_unique<UIElement>();
        buttons->AddClass("inspector-subtab-row");
        buttons->AddClass("inspector-subtab-wrap");
        UIElement* buttonRow = buttons.get();
        AddChild(std::move(buttons));
        for (const StarLookPreset& preset : StarLookPresets())
        {
            const StarLookPreset* chosen = &preset;
            auto button = std::make_unique<Button>();
            button->SetText(preset.Name);
            button->SetTooltip(preset.Tooltip);
            button->AddClass("segmented-button");
            button->AddClass("segmented-button-pair");
            button->SetOnClick([chosen, world, sky, notifications, undo](UIEvent&)
                               { OnLookClicked(*chosen, world, sky, notifications, undo); });
            m_Buttons.push_back(button.get());
            buttonRow->AddChild(std::move(button));
        }
        m_Custom = AddCaption(kCustomCaption);
        m_NoVolume = AddCaption(kNoVolumeCaption);
        Refresh();
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
    Label* AddCaption(const char* text)
    {
        auto caption = std::make_unique<Label>();
        caption->AddClass("inspector-field-caption");
        caption->SetText(text);
        Label* raw = caption.get();
        AddChild(std::move(caption));
        return raw;
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
        bool matched = false;
        const std::span<const StarLookPreset> presets = StarLookPresets();
        for (size_t i = 0; i < presets.size(); ++i)
        {
            const bool active = sky && MatchesStarLook(*sky, presets[i]);
            if (active)
                m_Buttons[i]->AddClass("segmented-button-selected");
            else
                m_Buttons[i]->RemoveClass("segmented-button-selected");
            matched = matched || active;
        }
        SetShown(m_Custom, !matched);
        SetShown(m_NoVolume, m_World && !HasGlobalPostProcessVolume(*m_World));
    }

    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Sky{};
    Editor::EditorChangeNotifications* m_Notifications = nullptr;
    Editor::EditorChangeNotifications::SubscriptionToken m_Subscription{};
    Editor::EditorChangeNotifications::SubscriptionToken m_WorldSubscription{};
    std::vector<Button*> m_Buttons;
    Label* m_Custom = nullptr;
    Label* m_NoVolume = nullptr;
};

} // namespace

void AddSkyStarLookRow(UIElement* parent, ECS::World* world, ECS::EntityHandle sky,
                       Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo)
{
    UIElement* row = InspectorUI::AddRow(parent);
    if (!row)
        return;
    InspectorUI::AddLabel(row, "Star look");
    row->AddChild(std::make_unique<StarLookField>(world, sky, notifications, undo));
}

} // namespace GameEngine
