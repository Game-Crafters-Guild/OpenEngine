#include "UndoRedo/SplineNetworkRecordCommand.h"

#include "Editor/Entities/EntityDisplayName.h"

#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "Mathematics/Geometry.h"
#include "SplineECS/SplineNetwork.h"

#include <algorithm>
#include <utility>

namespace GameEngine::Editor
{
namespace
{

using Components::SplineNetworkOverride;
using Components::SplineNetworkOverrides;
using Components::SplineNodeRule;
using Mathematics::GroundDistance;
using Mathematics::Vector3;
using SplineECS::SplineNetwork;
using SplineECS::SplineNode;

// Where the network last placed the record in `slot` of `holder`: the junction it rides on, or
// the place it is held. Empty when the network has not derived it.
std::optional<Vector3> RecordPosition(const SplineNetwork& network, const ECS::World& world, ECS::EntityHandle holder,
                                      uint32 slot)
{
    for (const SplineNode& node : network.GetNodes(world))
        if (node.Rule != SplineNodeRule::None && node.RuleHolder == holder && node.RuleSlot == slot)
            return node.Position;
    for (const SplineECS::SplineHeldRecord& held : network.GetHeldRecords(world))
        if (held.Holder == holder && held.Slot == slot)
            return held.LastPosition;
    return std::nullopt;
}

const SplineECS::SplineHeldRecord* FindHeld(const SplineNetwork& network, const ECS::World& world,
                                            ECS::EntityHandle holder, uint32 slot)
{
    for (const SplineECS::SplineHeldRecord& held : network.GetHeldRecords(world))
        if (held.Holder == holder && held.Slot == slot)
            return &held;
    return nullptr;
}

SplineNetworkOverride KeyedTo(const SplineNode& node, ECS::EntityHandle holder, SplineNetworkOverride record)
{
    const uint32 self = node.Members[0] == holder ? 0 : 1;
    record.ArcLengthMetres = node.ArcLengthMetres[self];
    record.OtherArcLengthMetres = node.ArcLengthMetres[1 - self];
    return record;
}

ECS::EntityHandle OtherMember(const SplineNode& node, ECS::EntityHandle member)
{
    return node.Members[0] == member ? node.Members[1] : node.Members[0];
}

} // namespace

SplineNetworkRecordCommand::SplineNetworkRecordCommand(ConstructionKey, ECS::World& world, ECS::EntityHandle holder,
                                                       std::string name)
    : m_World(&world)
    , m_Holder(holder)
    , m_Name(std::move(name))
{
}

std::unique_ptr<SplineNetworkRecordCommand> SplineNetworkRecordCommand::JoinAtGrade(ECS::World& world,
                                                                                    const SplineNode& node,
                                                                                    ECS::EntityHandle holder)
{
    if (node.Rule == SplineNodeRule::AtGrade || (node.Members[0] != holder && node.Members[1] != holder))
        return nullptr;
    const std::string name = "Join " + EntityDisplayName(world, holder) + " at Grade";
    const ECS::EntityHandle ruleHolder = node.Rule != SplineNodeRule::None ? node.RuleHolder : holder;
    auto command = std::make_unique<SplineNetworkRecordCommand>(ConstructionKey{}, world, ruleHolder, name);
    command->m_Position = node.Position;
    if (node.Rule != SplineNodeRule::None)
    {
        const auto* overrides = world.GetComponent<SplineNetworkOverrides>(ruleHolder);
        if (!overrides || node.RuleSlot >= Components::kMaxSplineNetworkOverrides)
            return nullptr;
        command->m_Before = overrides->Records[node.RuleSlot];
        command->m_LastSlot = node.RuleSlot;
        command->m_LastWritten = *command->m_Before;
    }
    SplineNetworkOverride after{};
    after.Other = OtherMember(node, ruleHolder);
    after.Rule = SplineNodeRule::AtGrade;
    command->m_After = KeyedTo(node, ruleHolder, after);
    return command;
}

std::unique_ptr<SplineNetworkRecordCommand> SplineNetworkRecordCommand::Attach(ECS::World& world, ECS::EntityHandle road,
                                                                               float32 roadArcLengthMetres,
                                                                               ECS::EntityHandle body,
                                                                               float32 bodyArcLengthMetres,
                                                                               const Vector3& bodyPoint)
{
    if (road == body || !world.IsValid(road) || !world.IsValid(body))
        return nullptr;
    const std::string name = "Attach " + EntityDisplayName(world, road) + " to " + EntityDisplayName(world, body);
    auto command = std::make_unique<SplineNetworkRecordCommand>(ConstructionKey{}, world, road, name);
    command->m_Position = bodyPoint;
    command->m_After = SplineNetworkOverride{body, roadArcLengthMetres, bodyArcLengthMetres, SplineNodeRule::Attach, {}};
    return command;
}

std::unique_ptr<SplineNetworkRecordCommand> SplineNetworkRecordCommand::Detach(ECS::World& world, const SplineNode& node)
{
    if (node.Rule != SplineNodeRule::Attach)
        return nullptr;
    const auto* overrides = world.GetComponent<SplineNetworkOverrides>(node.RuleHolder);
    if (!overrides || node.RuleSlot >= Components::kMaxSplineNetworkOverrides)
        return nullptr;
    const std::string name = "Detach " + EntityDisplayName(world, node.RuleHolder);
    auto command = std::make_unique<SplineNetworkRecordCommand>(ConstructionKey{}, world, node.RuleHolder, name);
    command->m_Position = node.Position;
    command->m_Before = overrides->Records[node.RuleSlot];
    command->m_LastSlot = node.RuleSlot;
    command->m_LastWritten = *command->m_Before;
    return command;
}

std::unique_ptr<SplineNetworkRecordCommand> SplineNetworkRecordCommand::Clear(ECS::World& world, ECS::EntityHandle holder,
                                                                              uint32 slot)
{
    const auto* overrides = world.GetComponent<SplineNetworkOverrides>(holder);
    if (!overrides || slot >= Components::kMaxSplineNetworkOverrides ||
        overrides->Records[slot].Rule == SplineNodeRule::None)
        return nullptr;
    auto command =
        std::make_unique<SplineNetworkRecordCommand>(ConstructionKey{}, world, holder, "Clear Junction Override");
    if (const SplineNetwork* network = SplineNetwork::TryGet())
        if (const std::optional<Vector3> position = RecordPosition(*network, world, holder, slot))
            command->m_Position = *position;
    command->m_Before = overrides->Records[slot];
    command->m_LastSlot = slot;
    command->m_LastWritten = *command->m_Before;
    return command;
}

std::unique_ptr<SplineNetworkRecordCommand> SplineNetworkRecordCommand::Keep(ECS::World& world, ECS::EntityHandle holder,
                                                                             uint32 slot, uint32 nodeId)
{
    const SplineNetwork* network = SplineNetwork::TryGet();
    const SplineECS::SplineHeldRecord* held = network ? FindHeld(*network, world, holder, slot) : nullptr;
    const SplineNode* node = network ? network->FindNode(world, nodeId) : nullptr;
    if (!held || !node ||
        std::find(held->CandidateNodes.begin(), held->CandidateNodes.end(), nodeId) == held->CandidateNodes.end())
        return nullptr;
    auto command = std::make_unique<SplineNetworkRecordCommand>(ConstructionKey{}, world, holder, "Keep Junction Rule");
    command->m_Position = held->LastPosition;
    command->m_Before = held->Record;
    command->m_After = KeyedTo(*node, holder, held->Record);
    command->m_KeepNodeId = nodeId;
    command->m_KeepPosition = node->Position;
    command->m_LastSlot = slot;
    command->m_LastWritten = held->Record;
    return command;
}

void SplineNetworkRecordCommand::Do()
{
    if (m_KeepNodeId == 0)
    {
        Apply(m_Before, m_After);
        return;
    }
    // A Keep lands the record through the network, which binds it to the chosen junction: a
    // plain write would be a load, matched by its keys, and stay held between the candidates.
    SplineNetwork* network = SplineNetwork::TryGet();
    const std::optional<uint32> slot = FindSlot(*m_Before);
    const SplineECS::SplineHeldRecord* held = network && slot ? FindHeld(*network, *m_World, m_Holder, *slot) : nullptr;
    if (!held)
    {
        Apply(m_Before, m_After);
        return;
    }
    uint32 nodeId = 0;
    float32 nearest = SplineECS::kNodeMatchMetres;
    for (const uint32 candidate : held->CandidateNodes)
    {
        if (candidate == m_KeepNodeId)
        {
            nodeId = candidate;
            break;
        }
        const SplineNode* node = network->FindNode(*m_World, candidate);
        if (node && GroundDistance(node->Position, m_KeepPosition) <= nearest)
        {
            nearest = GroundDistance(node->Position, m_KeepPosition);
            nodeId = candidate;
        }
    }
    if (nodeId == 0 || !network->KeepRecord(*m_World, m_Holder, *slot, nodeId))
    {
        Apply(m_Before, m_After);
        return;
    }
    m_KeepNodeId = nodeId;
    m_LastSlot = slot;
    m_LastWritten = m_World->GetComponent<SplineNetworkOverrides>(m_Holder)->Records[*slot];
}

void SplineNetworkRecordCommand::Undo()
{
    Apply(m_After, m_Before);
}

void SplineNetworkRecordCommand::Apply(const std::optional<SplineNetworkOverride>& from,
                                       const std::optional<SplineNetworkOverride>& to)
{
    if (!m_World || !m_World->IsValid(m_Holder))
        return;
    SplineNetworkOverrides overrides{};
    if (const auto* existing = m_World->GetComponent<SplineNetworkOverrides>(m_Holder))
        overrides = *existing;
    std::optional<uint32> slot;
    if (from)
    {
        slot = FindSlot(*from);
        if (!slot)
        {
            Logger::Log::Warning("{}: the junction record is no longer on {}; nothing changed", m_Name,
                           EntityDisplayName(*m_World, m_Holder));
            return;
        }
    }
    else
    {
        for (uint32 i = 0; i < Components::kMaxSplineNetworkOverrides && !slot; ++i)
            if (overrides.Records[i].Rule == SplineNodeRule::None)
                slot = i;
        if (!slot)
        {
            Logger::Log::Warning("{}: {} already holds {} junction rules; clear one first", m_Name,
                           EntityDisplayName(*m_World, m_Holder), Components::kMaxSplineNetworkOverrides);
            return;
        }
    }
    m_LastSlot = slot;
    m_LastWritten = to ? *to : SplineNetworkOverride{};
    // Through the network, so the next derivation finds the record by where its junction was and
    // re-keys it there, or holds it when the junction is gone.
    if (SplineNetwork* network = SplineNetwork::TryGet())
    {
        network->SetRecord(*m_World, m_Holder, *slot, m_LastWritten, PositionOf(m_LastWritten));
        return;
    }
    overrides.Records[*slot] = m_LastWritten;
    m_World->AddComponentImmediate(m_Holder, overrides);
}

std::optional<uint32> SplineNetworkRecordCommand::FindSlot(const SplineNetworkOverride& expected) const
{
    const auto* overrides = m_World->GetComponent<SplineNetworkOverrides>(m_Holder);
    if (!overrides)
        return std::nullopt;
    std::optional<uint32> found;
    uint32 count = 0;
    if (const SplineNetwork* network = SplineNetwork::TryGet())
    {
        for (uint32 slot = 0; slot < Components::kMaxSplineNetworkOverrides; ++slot)
        {
            const SplineNetworkOverride& record = overrides->Records[slot];
            if (record.Other != expected.Other || record.Rule != expected.Rule)
                continue;
            const std::optional<Vector3> position = RecordPosition(*network, *m_World, m_Holder, slot);
            if (position && GroundDistance(*position, m_Position) <= SplineECS::kNodeMatchMetres)
            {
                found = slot;
                ++count;
            }
        }
    }
    if (count == 1)
        return found;
    if (m_LastSlot && overrides->Records[*m_LastSlot] == m_LastWritten)
        return m_LastSlot;
    return std::nullopt;
}

const Vector3& SplineNetworkRecordCommand::PositionOf(const SplineNetworkOverride& record) const
{
    return m_KeepNodeId != 0 && m_After && record == *m_After ? m_KeepPosition : m_Position;
}

} // namespace GameEngine::Editor
