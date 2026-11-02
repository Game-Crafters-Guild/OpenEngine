#include "Prefabs/PrefabOverridesList/PrefabOverridesList.h"

#include "Prefabs/PrefabHierarchyRows.h"
#include "Prefabs/PrefabInstanceDescriptions.h"
#include "Prefabs/PrefabOverridesList/DiscardStaleOverrideCommand.h"
#include "Prefabs/PrefabOverridesList/PrefabOverridesListModel.h"
#include "Scene/SceneDocumentManager.h"
#include "UI/ControlLayoutLoader.h"
#include "UndoRedo/UndoRedoService.h"

#include "ECS/World.h"
#include "Scene/SceneIO.h"
#include "Types/ColorUtils.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Layout/PopupPlacement.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <array>
#include <cmath>
#include <format>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <utility>

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kLayoutAssetPath = "UI/controls/PrefabOverridesList/PrefabOverridesList.uxml";
constexpr const char* kGroupLayoutAssetPath = "UI/controls/PrefabOverridesList/PrefabOverrideGroup.uxml";
constexpr const char* kRowLayoutAssetPath = "UI/controls/PrefabOverridesList/PrefabOverrideRow.uxml";
constexpr const char* kStyleAssetPath = "UI/controls/PrefabOverridesList/PrefabOverridesList.css";
constexpr std::array<std::string_view, 8> kRequiredIds = {
    "prefab-overrides-title",  "prefab-overrides-against", "prefab-overrides-scroll",     "prefab-overrides-empty",
    "prefab-overrides-footer", "prefab-overrides-note",    "prefab-overrides-revert-all", "prefab-overrides-apply-all"};
constexpr std::array<std::string_view, 3> kGroupRequiredIds = {
    "prefab-override-group-glyph", "prefab-override-group-name", "prefab-override-group-pill"};
// Each action has a fixed slot in the row, in the order Revert, Apply, Discard.
constexpr std::array<std::string_view, 8> kRowRequiredIds = {
    "prefab-override-icon",   "prefab-override-label", "prefab-override-pill",   "prefab-override-value",
    "prefab-override-swatch", "prefab-override-revert", "prefab-override-apply", "prefab-override-discard"};
constexpr const char* kHiddenClass = "hidden";
constexpr float kAnchorGap = 4.0f;
constexpr float kEdgeMargin = 8.0f;
constexpr float kPlacementEpsilon = 0.25f;

// The icon class of each row kind; PrefabOverridesList.css gives each its image.
const char* KindIconClass(PrefabOverrideRowKind kind)
{
    switch (kind)
    {
        case PrefabOverrideRowKind::ChangedField: return "prefab-override-icon-field";
        case PrefabOverrideRowKind::AddedComponent:
        case PrefabOverrideRowKind::AddedEntity: return "prefab-override-icon-added";
        case PrefabOverrideRowKind::RemovedComponent:
        case PrefabOverrideRowKind::RemovedEntity: return "prefab-override-icon-removed";
        case PrefabOverrideRowKind::Stale: return "prefab-override-icon-stale";
    }
    return "prefab-override-icon-field";
}

// The pill of a row: the words of the Inspector's header pills for an added or removed component or entity. A
// stale row has no pill of its own: its group header has it.
const char* KindPill(PrefabOverrideRowKind kind)
{
    switch (kind)
    {
        case PrefabOverrideRowKind::AddedComponent:
        case PrefabOverrideRowKind::AddedEntity: return "Added";
        case PrefabOverrideRowKind::RemovedComponent:
        case PrefabOverrideRowKind::RemovedEntity: return "Removed";
        case PrefabOverrideRowKind::Stale:
        case PrefabOverrideRowKind::ChangedField: break;
    }
    return "";
}

// True for the kinds that the engine applies to the prefab and reverts on the instance.
bool HasActions(PrefabOverrideRowKind kind)
{
    return kind == PrefabOverrideRowKind::ChangedField || kind == PrefabOverrideRowKind::AddedComponent ||
           kind == PrefabOverrideRowKind::RemovedComponent;
}

std::string ApplyTooltip(const PrefabOverrideRow& row, const std::string& prefabName)
{
    switch (row.Kind)
    {
        case PrefabOverrideRowKind::AddedComponent:
            return std::format("Add {} to the prefab '{}'.", row.Label, prefabName);
        case PrefabOverrideRowKind::RemovedComponent:
            return std::format("Remove {} from the prefab '{}'.", row.Label, prefabName);
        default: return std::format("Write this value into the prefab '{}'.", prefabName);
    }
}

std::string RevertTooltip(const PrefabOverrideRow& row)
{
    switch (row.Kind)
    {
        case PrefabOverrideRowKind::AddedComponent: return std::format("Remove {} from this instance.", row.Label);
        case PrefabOverrideRowKind::RemovedComponent: return std::format("Restore {} on this instance.", row.Label);
        default: return "Set this field back to the prefab value.";
    }
}

// Shows each of `elements` when `shown`, else hides it.
void ShowWhen(bool shown, std::initializer_list<UIElement*> elements)
{
    for (UIElement* element : elements)
        shown ? element->RemoveClass(kHiddenClass) : element->AddClass(kHiddenClass);
}

// Shows `button` with `tooltip`; a press runs `run`.
void ShowAction(Button& button, const std::string& tooltip, std::function<void()> run)
{
    button.SetTooltip(tooltip);
    button.SetOnClick([run = std::move(run)](UIEvent&) { run(); });
    button.RemoveClass(kHiddenClass);
}

} // namespace

PrefabOverridesList::PrefabOverridesList(ECS::World& world, UndoRedoService* undo,
                                         EditorChangeNotifications* notifications, PrefabOverridesListContext context)
    : DismissablePopup(this), m_World(world), m_Undo(undo), m_Notifications(notifications),
      m_Context(std::move(context))
{
    AddClass("prefab-overrides-anchor");
    AddClass(kHiddenClass);
    // The list draws over the panels around the Inspector, as a context menu does.
    SetOverlayLayer(OverlayLayer::Dropdown);
    RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    std::unique_ptr<UIElement> panel = ControlLayoutLoader::Load(kLayoutAssetPath, kRequiredIds);
    if (!panel)
        return;
    m_Title = dynamic_cast<Label*>(panel->FindById("prefab-overrides-title"_sid));
    m_Against = dynamic_cast<Label*>(panel->FindById("prefab-overrides-against"_sid));
    m_Scroll = dynamic_cast<ScrollView*>(panel->FindById("prefab-overrides-scroll"_sid));
    m_Empty = dynamic_cast<Label*>(panel->FindById("prefab-overrides-empty"_sid));
    m_Note = dynamic_cast<Label*>(panel->FindById("prefab-overrides-note"_sid));
    m_RevertAll = dynamic_cast<Button*>(panel->FindById("prefab-overrides-revert-all"_sid));
    m_ApplyAll = dynamic_cast<Button*>(panel->FindById("prefab-overrides-apply-all"_sid));
    if (!m_Title || !m_Against || !m_Scroll || !m_Empty || !m_Note || !m_RevertAll || !m_ApplyAll)
        return;
    m_RevertAll->SetTooltip("Set every override back to the prefab value. One undo step.");
    m_RevertAll->SetOnClick([this](UIEvent&) { RevertTargets(m_Targets, "Revert all overrides"); });
    m_ApplyAll->SetOnClick([this](UIEvent&) { ApplyTargets(m_Targets, "Apply all overrides"); });
    m_Panel = panel.get();
    AddChild(std::move(panel));
}

PrefabOverridesList::~PrefabOverridesList() = default;

void PrefabOverridesList::Open(const UIElement& anchor, ECS::EntityHandle entity)
{
    if (!m_Panel)
        return;
    m_Anchor = &anchor;
    m_Entity = entity;
    m_Open = true;
    m_PlacedX = -1.0f;
    m_PlacedY = -1.0f;
    m_Side.reset();
    BuildRows();
    m_Scroll->SetScrollY(0.0f);
    RemoveClass(kHiddenClass);
    if (m_Context.Reopen)
        *m_Context.Reopen = {&m_World, FindOutermostInstanceRoot(m_World, entity)};
}

void PrefabOverridesList::Close()
{
    if (!m_Open)
        return;
    m_Open = false;
    m_Anchor = nullptr;
    AddClass(kHiddenClass);
    if (m_Context.Reopen)
        *m_Context.Reopen = {};
}

void PrefabOverridesList::Refresh()
{
    if (!m_Open)
        return;
    m_RowsStale = true;
    MarkDirty(UIElement::LayoutDirty);
}

void PrefabOverridesList::OnPostLayout()
{
    if (!m_Open)
        return;
    if (m_RowsStale)
        BuildRows();
    Place();
}

SceneDocumentManager* PrefabOverridesList::Document() const
{
    return m_Context.Document ? m_Context.Document() : nullptr;
}

void PrefabOverridesList::BuildRows()
{
    m_RowsStale = false;
    m_Scroll->GetViewport()->RemoveAllChildren();
    m_Targets.clear();
    SceneDocumentManager* document = Document();
    std::span<const Scene::SceneDegradation::Skip> skips;
    if (document && document->GetLastLoadDegraded().has_value())
        skips = document->GetLastLoadDegraded()->Census.skips;
    const PrefabOverridesListModel model = PrefabOverridesListModel::Describe(m_World, m_Entity, skips);
    const std::string prefabName = model.PrefabName.empty() ? std::string("Prefab") : model.PrefabName;
    const bool readOnly = m_Context.IsInPlayMode && m_Context.IsInPlayMode();
    m_Title->SetText(std::format("Overrides on '{}'", model.InstanceName));
    m_Title->SetTooltip(m_Title->GetText());
    m_Against->SetText("against " + prefabName);
    bool hasEntityRows = false;
    for (const PrefabOverrideGroup& group : model.Groups)
    {
        AppendGroupHeader(group);
        for (const PrefabOverrideRow& row : group.Rows)
        {
            hasEntityRows = hasEntityRows || row.Kind == PrefabOverrideRowKind::AddedEntity ||
                            row.Kind == PrefabOverrideRowKind::RemovedEntity;
            AppendRow(group.Entity, row, prefabName, readOnly);
        }
    }
    // An empty list is the title, the dim line and the foot, with no scroll space between them.
    const bool empty = model.Groups.empty();
    SetShownText(*m_Empty, empty ? std::string("No overrides on this instance.") : std::string());
    ShowWhen(!empty, {m_Scroll});
    const bool hasTargets = !m_Targets.empty();
    std::string note;
    if (readOnly)
        note = "Stop Play mode to apply or revert.";
    else if (hasTargets && hasEntityRows)
        note = "Revert all and Apply all leave added and removed entities as they are.";
    SetShownText(*m_Note, note);
    m_ApplyAll->SetText(std::format("Apply all to '{}'", prefabName));
    m_ApplyAll->SetTooltip(std::format("Write every override into the prefab '{}'. One undo step.", prefabName));
    // The foot keeps its buttons when nothing is left to apply or revert, so the list keeps its shape.
    ShowWhen(!readOnly, {m_RevertAll, m_ApplyAll});
    m_RevertAll->SetEnabled(hasTargets);
    m_ApplyAll->SetEnabled(hasTargets);
}

void PrefabOverridesList::AppendGroupHeader(const PrefabOverrideGroup& group)
{
    std::unique_ptr<UIElement> element = ControlLayoutLoader::Load(kGroupLayoutAssetPath, kGroupRequiredIds);
    if (!element)
        return;
    UIElement* glyph = element->FindById("prefab-override-group-glyph"_sid);
    auto* name = dynamic_cast<Label*>(element->FindById("prefab-override-group-name"_sid));
    UIElement* pill = element->FindById("prefab-override-group-pill"_sid);
    if (!glyph || !name || !pill)
        return;
    name->SetText(group.Name);
    name->SetTooltip(group.Name);
    // A stale group is an entity that the prefab does not have: the alert glyph, the name in amber and the pill.
    if (group.Stale)
    {
        glyph->RemoveClass("hierarchy-entity-prefab");
        glyph->AddClass("prefab-override-group-glyph-stale");
        name->AddClass("prefab-override-group-name-stale");
        pill->RemoveClass(kHiddenClass);
    }
    m_Scroll->AddContent(std::move(element));
}

void PrefabOverridesList::AppendRow(ECS::EntityHandle entity, const PrefabOverrideRow& row,
                                    const std::string& prefabName, bool readOnly)
{
    std::unique_ptr<UIElement> element = ControlLayoutLoader::Load(kRowLayoutAssetPath, kRowRequiredIds);
    if (!element)
        return;
    auto* label = dynamic_cast<Label*>(element->FindById("prefab-override-label"_sid));
    auto* pill = dynamic_cast<Label*>(element->FindById("prefab-override-pill"_sid));
    auto* value = dynamic_cast<Label*>(element->FindById("prefab-override-value"_sid));
    UIElement* swatch = element->FindById("prefab-override-swatch"_sid);
    UIElement* icon = element->FindById("prefab-override-icon"_sid);
    auto* apply = dynamic_cast<Button*>(element->FindById("prefab-override-apply"_sid));
    auto* revert = dynamic_cast<Button*>(element->FindById("prefab-override-revert"_sid));
    auto* discard = dynamic_cast<Button*>(element->FindById("prefab-override-discard"_sid));
    if (!label || !pill || !value || !swatch || !icon || !apply || !revert || !discard)
        return;
    icon->AddClass(KindIconClass(row.Kind));
    label->SetText(row.Label);
    label->SetTooltip(row.Label);
    SetShownText(*pill, KindPill(row.Kind));
    const bool showsValue = !row.PrefabValue.empty() || row.PrefabColor.has_value();
    SetShownText(*value, showsValue ? "prefab: " + row.PrefabValue : std::string());
    value->SetTooltip(row.PrefabValue);
    if (row.PrefabColor)
    {
        const ColorLinear& color = *row.PrefabColor;
        swatch->Style().Set(Style::BackgroundColor, ColorUtils::PackArgbRounded(color.r, color.g, color.b, 1.0f));
        swatch->RemoveClass(kHiddenClass);
    }
    if (row.Kind == PrefabOverrideRowKind::Stale)
    {
        label->AddClass("prefab-override-label-stale");
        if (!readOnly && Document())
            ShowAction(*discard,
                       "Save refuses this scene until you discard this override. The entity is not in the prefab. "
                       "One undo step.",
                       [this, skipIndex = row.SkipIndex]() { Discard(skipIndex); });
        m_Scroll->AddContent(std::move(element));
        return;
    }
    if (!HasActions(row.Kind))
    {
        m_Scroll->AddContent(std::move(element));
        return;
    }
    const Scene::PrefabOverrideTarget target{entity, row.Override};
    m_Targets.push_back(target);
    if (!readOnly)
    {
        ShowAction(*apply, ApplyTooltip(row, prefabName),
                   [this, target, undoName = std::format("Apply {} to prefab '{}'", row.Label, prefabName)]()
                   { ApplyTargets({&target, 1}, undoName); });
        ShowAction(*revert, RevertTooltip(row),
                   [this, target, undoName = "Revert " + row.Label]() { RevertTargets({&target, 1}, undoName); });
    }
    m_Scroll->AddContent(std::move(element));
}

void PrefabOverridesList::ApplyTargets(std::span<const Scene::PrefabOverrideTarget> targets,
                                       const std::string& undoName)
{
    // A copy: the apply can rebuild the rows that own `targets`.
    const std::vector<Scene::PrefabOverrideTarget> copy(targets.begin(), targets.end());
    ApplyPrefabOverridesAsOneStep(m_World, copy, m_Undo, m_Notifications, undoName);
    Refresh();
}

void PrefabOverridesList::RevertTargets(std::span<const Scene::PrefabOverrideTarget> targets,
                                        const std::string& undoName)
{
    const std::vector<Scene::PrefabOverrideTarget> copy(targets.begin(), targets.end());
    RevertPrefabOverridesAsOneStep(m_World, copy, m_Undo, m_Notifications, undoName);
    Refresh();
}

void PrefabOverridesList::Discard(std::size_t skipIndex)
{
    SceneDocumentManager* document = Document();
    if (!document)
        return;
    auto command = std::make_unique<DiscardStaleOverrideCommand>(
        *document, m_World, m_Notifications, FindOutermostInstanceRoot(m_World, m_Entity), skipIndex);
    if (m_Undo)
        m_Undo->Execute(std::move(command));
    else
        command->Do();
    Refresh();
}

void PrefabOverridesList::Place()
{
    UIManager* manager = GetOwnerManager();
    UIElement* root = manager ? manager->GetRootElement() : nullptr;
    if (!m_Anchor || !root)
        return;
    const Mathematics::Rect anchor = m_Anchor->GetWindowRect();
    const Mathematics::Rect window = root->GetWindowRect();
    const Mathematics::Rect origin = GetWindowRect();
    const float width = m_Panel->GetLayoutWidth();
    const float height = m_Panel->GetLayoutHeight();
    // The side is chosen once per Open, so the list does not jump when a revert shrinks it.
    const UI::Layout::AnchoredPopupPosition placed = UI::Layout::PlacePopupBelowOrAbove(
        {window.X, window.Y, window.Width, window.Height}, {anchor.X, anchor.Y, anchor.Width, anchor.Height}, width,
        height, kEdgeMargin, kAnchorGap, m_Side);
    if (height > 0.0f)
        m_Side = placed.Side;
    const float localX = placed.Position.X - origin.X;
    const float localY = placed.Position.Y - origin.Y;
    if (std::fabs(localX - m_PlacedX) < kPlacementEpsilon && std::fabs(localY - m_PlacedY) < kPlacementEpsilon)
        return;
    m_PlacedX = localX;
    m_PlacedY = localY;
    m_Panel->Style()
        .Set(Style::PositionLeft, StyleLength::Px(localX))
        .Set(Style::PositionTop, StyleLength::Px(localY));
    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

} // namespace GameEngine::Editor
