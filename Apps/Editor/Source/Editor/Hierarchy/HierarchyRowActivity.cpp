#include "Editor/Hierarchy/HierarchyRowActivity.h"

#include "ECS/Entity.h"
#include "ECS/World.h"
#include "SceneView/SceneViewEye.h"
#include "UI/Controls/TreeView.h"
#include "UI/UIElement.h"

namespace GameEngine::Editor
{
namespace
{
// A row's enable-state classes, read by the panel's sheet (UI/panels/HierarchyPanel.css) and, for
// the row's own class, the play-mode tint in theme/views.css.
constexpr const char* kEntityDisabledRowClass = "entity-disabled";
constexpr const char* kTitleSwitchedOffClass = "hierarchy-title-off";
constexpr const char* kTitleInactiveClass = "hierarchy-title-inactive";
constexpr const char* kIconSwitchedOffClass = "hierarchy-preview-icon-off";
// The Scene View eye's classes (UI/panels/HierarchyPanel.css): closed by the entity itself, or by an ancestor.
constexpr const char* kEyeClass = "hierarchy-eye";
constexpr const char* kEyeHiddenClass = "eye-closed";
constexpr const char* kEyeHiddenByParentClass = "eye-closed-by-parent";

void SetRowStateClass(UIElement& element, const char* className, bool present)
{
    if (present)
        element.AddClass(className);
    else
        element.RemoveClass(className);
}

// The title dims for an entity switched off itself and less for one inactive under a switched-off
// ancestor, with the state in words as its tooltip; the icon, the entity's own switch, dims only for
// an entity switched off itself. A pooled row drops what it carried for the entity it showed before.
void ApplyRowActivity(UIElement& row, ECS::World& world, const EntityActivity& activity)
{
    const bool inactive = activity.SwitchedOn && !activity.ActiveInHierarchy;
    SetRowStateClass(row, kEntityDisabledRowClass, !activity.SwitchedOn);
    for (const auto& child : row.GetChildren())
    {
        if (!child)
            continue;
        if (child->HasClass("tree-title"))
        {
            SetRowStateClass(*child, kTitleSwitchedOffClass, !activity.SwitchedOn);
            SetRowStateClass(*child, kTitleInactiveClass, inactive);
            child->SetTooltip(EntityActivityReason(world, activity));
        }
        else if (child->HasClass("hierarchy-preview-icon"))
        {
            SetRowStateClass(*child, kIconSwitchedOffClass, !activity.SwitchedOn);
        }
    }
}

// The eye shows its own state closed, an ancestor's dimmer, and an open eye only on hover; its
// tooltip says what a click does or which ancestor hides the entity.
void ApplyRowEye(UIElement& row, ECS::World& world, const SceneViewEyeState& eye)
{
    for (const auto& child : row.GetChildren())
    {
        if (!child || !child->HasClass(kEyeClass))
            continue;
        SetRowStateClass(*child, kEyeHiddenClass, eye.HiddenSelf);
        SetRowStateClass(*child, kEyeHiddenByParentClass, eye.HiddenByParent);
        child->SetTooltip(SceneViewEyeTooltip(world, eye));
    }
}
} // namespace

void HierarchyRowActivity::Present(ECS::World& world, ECS::EntityHandle entity, std::uint64_t treeId, UIElement& row)
{
    const EntityActivity activity = DescribeEntityActivity(world, entity);
    const SceneViewEyeState eye = DescribeSceneViewEye(world, entity);
    m_Shown[treeId] = {entity, activity, eye, m_Refreshes};
    ApplyRowActivity(row, world, activity);
    ApplyRowEye(row, world, eye);
}

void HierarchyRowActivity::Refresh(ECS::World& world, const TreeView& tree, TreeChangeTrackingProvider& provider)
{
    const std::size_t version = world.GetStructuralChangeVersion();
    if (m_HasStructuralVersion && version == m_StructuralVersion)
        return;
    m_StructuralVersion = version;
    m_HasStructuralVersion = true;

    ++m_Refreshes;
    m_Moved.clear();
    tree.CollectBoundIds(m_Held);
    for (const std::uint64_t id : m_Held)
    {
        const auto shown = m_Shown.find(id);
        if (shown == m_Shown.end())
            continue;
        shown->second.HeldAtRefresh = m_Refreshes;
        if (!world.IsValid(shown->second.Entity))
            continue;
        const EntityActivity activity = DescribeEntityActivity(world, shown->second.Entity);
        const SceneViewEyeState eye = DescribeSceneViewEye(world, shown->second.Entity);
        if (activity == shown->second.Activity && eye == shown->second.Eye)
            continue;
        shown->second.Activity = activity;
        shown->second.Eye = eye;
        m_Moved.push_back(id);
    }
    std::erase_if(m_Shown, [this](const auto& entry) { return entry.second.HeldAtRefresh != m_Refreshes; });
    if (!m_Moved.empty())
        provider.MarkChangedBatch(m_Moved);
}

} // namespace GameEngine::Editor
