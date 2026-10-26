#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "Components/Spline/SplineNetworkOverrides.h"
#include "ECS/Entity.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <memory>
#include <optional>
#include <string>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::SplineECS
{
struct SplineNode;
}

namespace GameEngine::Editor
{

/// One edit of one junction record (a slot of SplineNetworkOverrides) as one undo step: Attach,
/// Detach, Join at Grade, Clear and Keep. The step journals only the record it changes, keyed by
/// its holder, its partner, its rule and where its junction was; undo and redo find the record
/// again through the spline network by that position and write it back through the network,
/// which matches it by that position, so an edit elsewhere in between (an upstream knot drag
/// that re-keyed every arc length) disturbs no other record. A record whose junction is gone
/// when the step replays keeps its journaled keys, and the network holds and reports it.
class SplineNetworkRecordCommand final : public IEditorCommand
{
  private:
    // Only the factories below construct a step: each checks that its record exists.
    struct ConstructionKey
    {
        explicit ConstructionKey() = default;
    };

  public:
    SplineNetworkRecordCommand(ConstructionKey, ECS::World& world, ECS::EntityHandle holder, std::string name);

    /// "Join <road> at Grade": an AtGrade rule on `node`. A node that already has a rule changes
    /// that record where it sits; a node with none gets a new record on `holder`, one of the
    /// node's two members. Null when the node is joined at grade already or `holder` is not
    /// one of its members.
    static std::unique_ptr<SplineNetworkRecordCommand> JoinAtGrade(ECS::World& world, const SplineECS::SplineNode& node,
                                                                   ECS::EntityHandle holder);

    /// "Attach <road> to <other>": the end of `road` nearest `roadArcLengthMetres` attaches to
    /// `body` at `bodyArcLengthMetres`, which lies at `bodyPoint` in the world. Null when `road`
    /// is `body` or either is not an entity of `world`.
    static std::unique_ptr<SplineNetworkRecordCommand> Attach(ECS::World& world, ECS::EntityHandle road,
                                                              float32 roadArcLengthMetres, ECS::EntityHandle body,
                                                              float32 bodyArcLengthMetres,
                                                              const Mathematics::Vector3& bodyPoint);

    /// "Detach <road>": removes the Attach record that made `node`. Null when no Attach record
    /// made it.
    static std::unique_ptr<SplineNetworkRecordCommand> Detach(ECS::World& world, const SplineECS::SplineNode& node);

    /// "Clear Junction Override": removes the record in `slot` of `holder`, on a junction or
    /// held. Null when the slot is empty.
    static std::unique_ptr<SplineNetworkRecordCommand> Clear(ECS::World& world, ECS::EntityHandle holder, uint32 slot);

    /// "Keep Junction Rule": lands the held record in `slot` of `holder` on `nodeId`, one of the
    /// junctions it cannot choose between. Null when the record is not held with that candidate.
    static std::unique_ptr<SplineNetworkRecordCommand> Keep(ECS::World& world, ECS::EntityHandle holder, uint32 slot,
                                                            uint32 nodeId);

    const char* GetName() const override { return m_Name.c_str(); }
    const char* GetTypeName() const override { return "SplineNetworkRecordCommand"; }

    void Do() override;
    void Undo() override;

  private:
    // Replaces the record `from` (absent: a free slot) with `to` (absent: an empty slot).
    void Apply(const std::optional<Components::SplineNetworkOverride>& from,
               const std::optional<Components::SplineNetworkOverride>& to);
    // The slot holding the record this step journals as `expected`: the one record of that
    // partner and rule whose junction the network places within reach of m_Position, else the
    // slot this step last wrote when the network has not derived since.
    std::optional<uint32> FindSlot(const Components::SplineNetworkOverride& expected) const;
    // Where the junction of `record` is: the kept junction for a Keep's landed record, else
    // where the step was journaled.
    const Mathematics::Vector3& PositionOf(const Components::SplineNetworkOverride& record) const;

    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Holder{};
    std::string m_Name;
    std::optional<Components::SplineNetworkOverride> m_Before;
    std::optional<Components::SplineNetworkOverride> m_After;
    // Where the record's junction is, in world space: the key undo and redo find it by.
    Mathematics::Vector3 m_Position{};
    // Keep: the junction the record lands on, and where it was.
    uint32 m_KeepNodeId = 0;
    Mathematics::Vector3 m_KeepPosition{};
    std::optional<uint32> m_LastSlot;
    Components::SplineNetworkOverride m_LastWritten{};
};

} // namespace GameEngine::Editor
