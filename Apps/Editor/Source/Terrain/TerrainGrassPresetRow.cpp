#include "Terrain/TerrainGrassPresetRow.h"

#include "Components/Terrain/TerrainGrass.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "TerrainGrass/TerrainGrassPresets.h"
#include "UI/Controls/Dropdown.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

constexpr const char* kPresetTooltip =
    "The kind of field: sets the colors, blade height and width, density and tilt below, in one undo step. "
    "Every setting stays editable. Reads Custom when one of them differs from the preset.";
constexpr const char* kCustomText = "Custom";
// The list index that shows Custom: no entry selected.
constexpr int kCustomIndex = -1;

// One entry of the Preset list: the stored preset and what the list shows for it.
struct GrassPresetEntry
{
    Components::TerrainGrassPreset Preset;
    const char* Name;
    /// One line under the name: what the field looks like.
    const char* Description;
    /// The CSS class of the preset's picture (theme/option-pictures.css).
    const char* PictureClass;
};

// In the list's order, Meadow (the component's defaults) first.
constexpr GrassPresetEntry kGrassPresetEntries[] = {
    {Components::TerrainGrassPreset::Meadow, "Meadow", "The default. Mid-height and leaf green.",
     "option-picture--grass-meadow"},
    {Components::TerrainGrassPreset::Lawn, "Lawn", "Short, dense and upright.", "option-picture--grass-lawn"},
    {Components::TerrainGrassPreset::Steppe, "Steppe", "Sparse, tall and dry.", "option-picture--grass-steppe"},
};

std::vector<Dropdown::Option> PresetOptions()
{
    std::vector<Dropdown::Option> options;
    for (const GrassPresetEntry& entry : kGrassPresetEntries)
        options.push_back({entry.Name, entry.Name, {}, {}, entry.PictureClass, entry.Description});
    return options;
}

// The list index of the preset the grass is on, or kCustomIndex. Reconciled first, so a scene whose
// dials were written by another path reads Custom.
int ShownPresetIndex(const Components::TerrainGrass* grass)
{
    if (!grass)
        return kCustomIndex;
    Components::TerrainGrass shown = *grass;
    TerrainGrass::ReconcileTerrainGrassPreset(shown);
    for (size_t i = 0; i < std::size(kGrassPresetEntries); ++i)
    {
        if (kGrassPresetEntries[i].Preset == shown.Preset)
            return static_cast<int>(i);
    }
    return kCustomIndex;
}

// The Preset list, kept in step with the grass's dials.
class GrassPresetDropdown final : public Dropdown
{
  public:
    GrassPresetDropdown(ECS::World* world, ECS::EntityHandle terrain, Editor::EditorChangeNotifications* notifications,
                        Editor::UndoRedoService* undo, std::function<void()> requestRefresh)
        : m_World(world)
        , m_Terrain(terrain)
        , m_Notifications(notifications)
        , m_Undo(undo)
        , m_RequestRefresh(std::move(requestRefresh))
    {
        AddClass("inspector-dropdown");
        SetNoSelectionText(kCustomText);
        SetOptions(PresetOptions());
        Refresh();
        SetOnValueChanged([this](const std::string&) { OnPresetPicked(); });
        if (m_Notifications)
            m_Subscription = m_Notifications->SubscribeComponentChanged(
                [this](const Editor::EditorChangeNotifications::ComponentChangedEvent& event) { OnChanged(event); });
    }

    ~GrassPresetDropdown() override
    {
        if (m_Notifications && m_Subscription)
            m_Notifications->Unsubscribe(m_Subscription);
    }

  private:
    void OnPresetPicked()
    {
        const int index = GetSelectedIndex();
        if (!m_World || index < 0 || static_cast<size_t>(index) >= std::size(kGrassPresetEntries))
            return;
        const GrassPresetEntry& entry = kGrassPresetEntries[static_cast<size_t>(index)];
        InspectorDrag::CommitComponentWithUndo<Components::TerrainGrass>(
            m_World, m_Terrain, m_Notifications, m_Undo, std::string("Grass preset: ") + entry.Name,
            [&entry](Components::TerrainGrass& grass) { TerrainGrass::ApplyTerrainGrassPreset(grass, entry.Preset); });
        // The dial rows show the values they were built with: rebuild them so they show the preset's.
        if (m_RequestRefresh)
            m_RequestRefresh();
    }

    void OnChanged(const Editor::EditorChangeNotifications::ComponentChangedEvent& event)
    {
        if (event.world == m_World && event.entity == m_Terrain &&
            event.componentType == ECS::GetComponentTypeId<Components::TerrainGrass>())
            Refresh();
    }

    void Refresh()
    {
        // An open list belongs to the user: the selection is not moved under the pointer.
        if (IsMenuOpen())
            return;
        const auto* grass = m_World ? m_World->GetComponent<Components::TerrainGrass>(m_Terrain) : nullptr;
        SetSelectedIndexWithoutNotify(ShownPresetIndex(grass));
    }

    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Terrain{};
    Editor::EditorChangeNotifications* m_Notifications = nullptr;
    Editor::UndoRedoService* m_Undo = nullptr;
    std::function<void()> m_RequestRefresh;
    Editor::EditorChangeNotifications::SubscriptionToken m_Subscription{};
};

} // namespace

void AddTerrainGrassPresetRow(UIElement* parent, ECS::World* world, ECS::EntityHandle terrain,
                              Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo,
                              std::function<void()> requestRefresh)
{
    InspectorUI::AddFieldRow(parent, "Preset",
                             std::make_unique<GrassPresetDropdown>(world, terrain, notifications, undo,
                                                                   std::move(requestRefresh)),
                             kPresetTooltip);
}

} // namespace GameEngine
