#pragma once

#include "SceneView/SceneViewport/SceneViewportBinding.h"

#include "ECS/Entity.h"
#include "Mathematics/Vector3.h"
#include "UI/Interaction/Types.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine
{

/// The world edits of one Scene View viewport: each lands in the world it is given and records
/// its undo step, its change notice and its dirty mark through the viewport's binding.
class SceneViewportEdits
{
public:
    /// The hierarchy selection on one side of a drop: the selected items and the anchor.
    struct DropSelection
    {
        std::vector<UI::Interaction::ItemId> Items;
        UI::Interaction::ItemId Anchor = 0;
    };
    using ApplySelectionFn = std::function<void(const std::vector<UI::Interaction::ItemId>&, UI::Interaction::ItemId)>;

    SceneViewportEdits(ECS::World& world, const SceneViewportBinding& binding);

    /// Creates a measure root named "Measure" at the midpoint, with "Measure Start" and "Measure End" children
    /// at the two world points, and records it as one undo step. colorArgb is 0xAARRGGBB.
    /// Returns an invalid handle when the two points are the same.
    ECS::EntityHandle CreateMeasure(const Mathematics::Vector3& start,
                                    const Mathematics::Vector3& end,
                                    bool is2D,
                                    std::uint32_t colorArgb);

    /// Records the roots of a drop as one undo step that also restores the selection before and after it.
    void RecordDrop(std::vector<ECS::EntityHandle> roots,
                    const DropSelection& before,
                    const DropSelection& after,
                    ApplySelectionFn applySelection);

    /// Sends the structure change notice for the world and marks the scene dirty.
    void MarkStructureChanged();

private:
    ECS::World& m_World;
    SceneViewportBinding m_Binding;
};

} // namespace GameEngine
