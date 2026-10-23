#pragma once

#include "SmartShape2D/SmartShapeCut.h"
#include "ECS/ECS.h"
#include "Mathematics/Vector3.h"

#include <vector>

namespace GameEngine::ECS { class World; }

namespace GameEngine::SmartShape2D
{
/// Session-local terrain history. Calls require exclusive world access. Restores
/// damage only, not physics poses. World replacement discards the history.
class ShapeDestructionHistory
{
public:
    uint32 Explode(ECS::World& world, Mathematics::Vector3 center, float radius);
    void Reset(ECS::World& world);
    bool Undo(ECS::World& world);
    bool Redo(ECS::World& world);

private:
    struct Footprint
    {
        ECS::EntityHandle Owner;
        Components::SmartShapeCut Cut;
        bool operator==(const Footprint&) const = default;
    };
    using Snapshot = std::vector<Footprint>;
    static Snapshot Capture(ECS::World& world);
    static void Apply(ECS::World& world, const Snapshot& snapshot);
    void Synchronize(ECS::World& world);
    void Commit(ECS::World& world);
    uint64 m_WorldId = 0;
    uint64 m_ResetGeneration = 0;
    std::vector<Snapshot> m_States;
    size_t m_Position = 0;
};
}
