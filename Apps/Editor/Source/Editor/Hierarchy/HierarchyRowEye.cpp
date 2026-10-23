#include "Editor/Hierarchy/HierarchyRowEye.h"

#include "SceneView/SceneViewEye.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <memory>
#include <utility>

namespace GameEngine::Editor
{

HierarchyRowEye::HierarchyRowEye(HierarchyRowEyeHost host) : m_Host(std::move(host))
{
}

void HierarchyRowEye::AddTo(UIElement& row)
{
    auto eye = std::make_unique<UIElement>();
    eye->AddClass("hierarchy-eye");
    eye->SetDisableClipCulling(true);
    UIElement* rowPtr = &row;
    eye->RegisterEventHandler(kEventMouseDown, [this, rowPtr](UIEvent& e) { OnMouseDown(*rowPtr, e); });
    row.AddChild(std::move(eye));
}

void HierarchyRowEye::Present(UIElement& row, ECS::EntityHandle entity, bool hasChildren)
{
    ECS::World* world = m_Host.World ? m_Host.World() : nullptr;
    const bool shown = hasChildren || (world && DescribeSceneViewEye(*world, entity).HasMesh);
    for (const auto& child : row.GetChildren())
    {
        if (child && child->HasClass("hierarchy-eye"))
            UI::Layout::SetElementHidden(*child, !shown);
    }
}

void HierarchyRowEye::OnMouseDown(UIElement& row, UIEvent& e)
{
    if (e.Button != 0)
        return;
    // The eye is not a row click: it neither selects nor starts a drag.
    e.Stop();
    ECS::World* world = m_Host.World ? m_Host.World() : nullptr;
    if (!world || !m_Host.RowEntity || !m_Host.ClickTargets)
        return;
    const ECS::EntityHandle entity = m_Host.RowEntity(row);
    if (!entity.IsValid())
        return;
    // Descendants follow through the derived state a frame later.
    const bool hidden = !DescribeSceneViewEye(*world, entity).HiddenSelf;
    SetHiddenInSceneView(*world, m_Host.ClickTargets(entity), hidden);
    if (m_Host.RowChanged)
        m_Host.RowChanged(row, entity);
}

} // namespace GameEngine::Editor
